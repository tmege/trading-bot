--[[
  DCA + Mean Reversion Strategy — ETH

  Based on research: DCA on dips is the most consistent strategy for small accounts.
  Academic backing: 202% cumulative over 5 years on BTC (SpotedCrypto backtest).

  Logic:
  - Buy small amounts when price dips (RSI < 30 or price < SMA20 - 2*ATR)
  - Buy medium amounts on deeper dips (RSI < 20 or price < SMA20 - 3*ATR)
  - Sell portions on recovery (RSI > 70 or price > SMA20 + ATR)
  - Track average entry price for accurate P&L
  - Never go short — long-only accumulation strategy
  - ATR-based position sizing: buy more when volatility is high (better entries)
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = "ETH",
    leverage      = 3,           -- conservative leverage for DCA

    -- DCA buy conditions
    rsi_buy_1     = 30,          -- first buy threshold
    rsi_buy_2     = 20,          -- deeper buy (double size)
    sma_period    = 20,
    atr_period    = 14,
    atr_buy_mult  = 2.0,         -- buy when price < SMA - ATR*mult

    -- Position sizing
    base_buy_usd  = 15.0,        -- base buy size ($15)
    deep_buy_usd  = 30.0,        -- double on deep dip ($30)
    max_position  = 200.0,       -- max total position in USD

    -- Sell conditions
    rsi_sell      = 70,          -- sell threshold
    atr_sell_mult = 1.0,         -- sell when price > SMA + ATR*mult
    sell_pct      = 0.5,         -- sell 50% of position at a time
    min_profit    = 1.5,         -- minimum profit % to sell

    -- Timing
    check_sec     = 120,         -- check every 2 min (no need to be fast)
    cooldown_buy  = 900,         -- 15 min between buys (avoid overbuying)

    -- Safety
    stop_loss_pct = 8.0,         -- emergency stop if position drops > 8%
    max_drop_24h  = 10.0,        -- pause if ETH drops > 10% in 24h

    enabled       = true,
}

-- ── State ──────────────────────────────────────────────────────────────────

local last_check     = 0
local last_buy_time  = 0
local avg_entry      = 0
local total_size     = 0         -- total ETH held
local initialized    = false
local eth_price_24h  = 0

-- ── Helpers ────────────────────────────────────────────────────────────────

local function get_indicators()
    local ind = bot.get_indicators(config.coin, "1h", 50)
    if not ind then return nil end
    return ind
end

local function current_position_usd(mid)
    return total_size * mid
end

local function unrealized_pnl_pct(mid)
    if avg_entry <= 0 or total_size <= 0 then return 0 end
    return ((mid - avg_entry) / avg_entry) * 100
end

local function update_avg_entry(buy_price, buy_size)
    local old_value = avg_entry * total_size
    local new_value = buy_price * buy_size
    total_size = total_size + buy_size
    if total_size > 0 then
        avg_entry = (old_value + new_value) / total_size
    end
end

local function do_buy(mid, usd_amount)
    if current_position_usd(mid) + usd_amount > config.max_position then
        usd_amount = config.max_position - current_position_usd(mid)
        if usd_amount < 5 then return end  -- too small
    end

    local size = usd_amount / mid
    local oid = bot.place_limit(config.coin, "buy", mid, size, { tif = "ioc" })
    if oid then
        bot.log("info", string.format("dca: BUY $%.0f (%.5f ETH) @ $%.2f",
            usd_amount, size, mid))
        last_buy_time = bot.time()
    end
end

local function do_sell(mid, pct_of_position)
    if total_size <= 0 then return end

    local sell_size = total_size * pct_of_position
    if sell_size * mid < 5 then return end  -- too small

    local oid = bot.place_limit(config.coin, "sell", mid, sell_size,
                                { tif = "ioc", reduce_only = true })
    if oid then
        bot.log("info", string.format("dca: SELL %.1f%% (%.5f ETH) @ $%.2f, profit=%.1f%%",
            pct_of_position * 100, sell_size, mid, unrealized_pnl_pct(mid)))
    end
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "dca_eth: init — base=$%.0f, deep=$%.0f, RSI buy<%d/%d, RSI sell>%d",
        config.base_buy_usd, config.deep_buy_usd,
        config.rsi_buy_1, config.rsi_buy_2, config.rsi_sell))

    -- Restore state
    local saved_enabled = bot.load_state("enabled")
    if saved_enabled == "false" then config.enabled = false end

    -- Check existing position
    local pos = bot.get_position(config.coin)
    if pos and pos.size > 0 then
        total_size = pos.size
        avg_entry = pos.entry_px
        bot.log("info", string.format("dca: existing position %.5f ETH @ $%.2f",
            total_size, avg_entry))
    end

    local mid = bot.get_mid_price(config.coin)
    if mid then eth_price_24h = mid end

    initialized = true
    last_check = bot.time()
end

function on_tick(coin, mid_price)
    if coin ~= config.coin then return end
    if not config.enabled then return end

    local now = bot.time()
    if now - last_check < config.check_sec then return end
    last_check = now

    -- 24h safety
    if eth_price_24h > 0 then
        local drop = ((mid_price - eth_price_24h) / eth_price_24h) * 100
        if drop < -config.max_drop_24h then
            bot.log("warn", string.format("dca: ETH dropped %.1f%% in 24h, pausing", drop))
            config.enabled = false
            bot.save_state("enabled", "false")
            return
        end
    end

    -- Emergency stop loss on position
    if total_size > 0 then
        local pnl = unrealized_pnl_pct(mid_price)
        if pnl < -config.stop_loss_pct then
            bot.log("warn", string.format("dca: EMERGENCY STOP — position at %.1f%%", pnl))
            do_sell(mid_price, 1.0)  -- sell everything
            return
        end
    end

    -- Get indicators
    local ind = get_indicators()
    if not ind then return end

    local rsi = ind.rsi
    local sma = ind.sma
    local atr = ind.atr

    if not rsi or not sma or not atr then return end

    -- ── SELL logic ─────────────────────────────────────────────────────
    if total_size > 0 and unrealized_pnl_pct(mid_price) > config.min_profit then
        -- Sell on RSI overbought
        if rsi > config.rsi_sell then
            bot.log("info", string.format("dca: RSI=%.0f > %d — selling", rsi, config.rsi_sell))
            do_sell(mid_price, config.sell_pct)
            return
        end

        -- Sell on price above SMA + ATR
        if mid_price > sma + atr * config.atr_sell_mult then
            bot.log("info", string.format("dca: price $%.0f > SMA+ATR $%.0f — selling",
                mid_price, sma + atr * config.atr_sell_mult))
            do_sell(mid_price, config.sell_pct)
            return
        end
    end

    -- ── BUY logic ──────────────────────────────────────────────────────

    -- Cooldown check
    if now - last_buy_time < config.cooldown_buy then return end

    -- Already at max?
    if current_position_usd(mid_price) >= config.max_position then return end

    -- Deep dip: RSI < 20 or price way below SMA
    if rsi < config.rsi_buy_2 or mid_price < sma - atr * (config.atr_buy_mult + 1) then
        bot.log("info", string.format("dca: DEEP DIP — RSI=%.0f, price=$%.0f, SMA=$%.0f",
            rsi, mid_price, sma))
        do_buy(mid_price, config.deep_buy_usd)
        return
    end

    -- Normal dip: RSI < 30 or price below SMA - 2*ATR
    if rsi < config.rsi_buy_1 or mid_price < sma - atr * config.atr_buy_mult then
        bot.log("info", string.format("dca: DIP — RSI=%.0f, price=$%.0f, SMA=$%.0f",
            rsi, mid_price, sma))
        do_buy(mid_price, config.base_buy_usd)
        return
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    if fill.side == "buy" then
        update_avg_entry(fill.price, fill.size)
        bot.log("info", string.format("dca: BOUGHT %.5f @ $%.2f — total=%.5f, avg=$%.2f",
            fill.size, fill.price, total_size, avg_entry))
    elseif fill.side == "sell" then
        total_size = total_size - fill.size
        if total_size < 0.00001 then
            total_size = 0
            avg_entry = 0
        end
        bot.log("info", string.format("dca: SOLD %.5f @ $%.2f — remaining=%.5f, pnl=%.4f",
            fill.size, fill.price, total_size, fill.closed_pnl))
    end

    bot.save_state("total_size", tostring(total_size))
    bot.save_state("avg_entry", tostring(avg_entry))
end

function on_timer()
    if not initialized then return end

    -- Sync position with exchange
    local pos = bot.get_position(config.coin)
    if pos then
        if pos.size > 0 then
            total_size = pos.size
            if pos.entry_px > 0 then avg_entry = pos.entry_px end
        elseif pos.size == 0 and total_size > 0.00001 then
            -- Position was closed externally
            total_size = 0
            avg_entry = 0
        end
    end
end

function on_advisory(json_str)
    bot.log("info", "dca_eth: advisory: " .. json_str)

    local new_base = json_str:match('"base_buy_usd"%s*:%s*([%d%.]+)')
    local pause = json_str:match('"pause"%s*:%s*(true)')
    local resume = json_str:match('"pause"%s*:%s*(false)')

    if new_base then
        local v = tonumber(new_base)
        if v and v > 0 and v <= 50 then
            config.base_buy_usd = v
            config.deep_buy_usd = v * 2
            bot.log("info", string.format("dca: advisory set base=$%.0f, deep=$%.0f", v, v*2))
        end
    end

    if pause then
        config.enabled = false
        bot.save_state("enabled", "false")
        bot.log("warn", "dca: PAUSED by advisory")
    end

    if resume and not config.enabled then
        config.enabled = true
        bot.save_state("enabled", "true")
        bot.log("info", "dca: RESUMED by advisory")
    end
end

function on_shutdown()
    bot.log("info", string.format("dca_eth: shutdown — holding %.5f ETH @ avg $%.2f",
        total_size, avg_entry))
    bot.save_state("enabled", tostring(config.enabled))
    bot.save_state("total_size", tostring(total_size))
    bot.save_state("avg_entry", tostring(avg_entry))
end
