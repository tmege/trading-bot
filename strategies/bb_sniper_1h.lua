--[[
  BB Sniper — High Win Rate Mean Reversion

  Concept: Wait for EXTREME oversold/overbought (RSI < 20 or > 80),
  take quick profit back to BB middle. Wide SL gives room to breathe.

  Target: 75-85% win rate, small wins, rare bigger losses.
  Math: 80% × $1 win - 20% × $2.5 loss = +$0.30 per trade

  Rules:
  - LONG:  price < BB lower AND RSI < 20 → TP at BB middle
  - SHORT: price > BB upper AND RSI > 80 → TP at BB middle
  - SL: 2.5x ATR (wide, lets price breathe)
  - ADX < 30 (no trend = mean reversion works)
  - Cooldown: 4h (few trades, high conviction only)
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = COIN or "ETH",

    -- Entry filters — extreme only
    rsi_oversold  = 20,        -- very extreme (not 30)
    rsi_overbought = 80,       -- very extreme (not 70)
    adx_max       = 30,        -- skip if trending (MR doesn't work in trends)
    min_bb_width  = 0.015,     -- need minimum 1.5% BB width for range

    -- SL: wide (let price breathe)
    sl_atr_mult   = 2.5,       -- 2.5x ATR = wide SL
    sl_pct_min    = 1.5,       -- floor 1.5%
    sl_pct_max    = 4.0,       -- cap 4%

    -- TP: tight (quick profit at BB middle)
    -- TP is DYNAMIC: distance to BB middle at entry time
    tp_min_pct    = 0.5,       -- minimum 0.5% TP (skip if BB too narrow)
    tp_max_pct    = 3.0,       -- cap 3% TP

    -- Position sizing
    equity_pct    = 0.20,      -- 20% of equity per trade
    max_size      = 200.0,
    entry_size    = 40.0,

    -- Timing
    check_sec     = 60,
    cooldown_sec  = 14400,     -- 4h between trades (very selective)
    max_hold_sec  = 28800,     -- max 8h (MR should resolve fast)

    enabled       = true,
}

local instance_name = "bb_sniper_1h_" .. config.coin:lower()

local function get_trade_size()
    local acct = bot.get_account_value()
    if acct and acct > 0 then
        return math.min(acct * config.equity_pct, config.max_size)
    end
    return config.entry_size
end

-- ── State ──────────────────────────────────────────────────────────────────

local last_check     = 0
local last_trade     = 0
local in_position    = false
local position_side  = nil
local entry_price    = 0
local entry_time     = 0
local sl_oid         = nil
local tp_oid         = nil
local entry_oid      = nil
local entry_placed_at = 0
local ENTRY_TIMEOUT  = 60
local trade_count    = 0
local win_count      = 0

-- ── Helpers ────────────────────────────────────────────────────────────────

local function place_entry(side, mid)
    if in_position then return nil end

    if entry_oid then
        bot.cancel(config.coin, entry_oid)
        entry_oid = nil
    end

    local price = side == "long"
        and mid * 0.9998
        or  mid * 1.0002
    local trade_usd = get_trade_size()
    local size = trade_usd / mid
    local order_side = side == "long" and "buy" or "sell"
    local oid = bot.place_limit(config.coin, order_side, price, size, { tif = "alo" })
    if oid then
        entry_oid = oid
        entry_placed_at = bot.time()
        bot.log("info", string.format("%s: ENTRY %s @ $%.2f ($%.0f)",
            instance_name, string.upper(side), price, trade_usd))
        return oid
    end
    return nil
end

local function place_sl_tp(fill_price, side, pos_size, bb_mid)
    -- SL: ATR-based, wide
    local ind = bot.get_indicators(config.coin, "1h", 50, fill_price)
    local atr = (ind and ind.atr and ind.atr > 0) and ind.atr or (fill_price * 0.015)

    local sl_pct = (config.sl_atr_mult * atr / fill_price) * 100
    sl_pct = math.max(config.sl_pct_min, math.min(config.sl_pct_max, sl_pct))

    -- TP: distance to BB middle (dynamic)
    local tp_pct
    if side == "long" then
        tp_pct = ((bb_mid - fill_price) / fill_price) * 100
    else
        tp_pct = ((fill_price - bb_mid) / fill_price) * 100
    end
    tp_pct = math.max(config.tp_min_pct, math.min(config.tp_max_pct, tp_pct))

    local sl_price, tp_price
    if side == "long" then
        sl_price = fill_price * (1 - sl_pct / 100)
        tp_price = fill_price * (1 + tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "sell", sl_price, pos_size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "sell", tp_price, pos_size, tp_price, "tp")
    else
        sl_price = fill_price * (1 + sl_pct / 100)
        tp_price = fill_price * (1 - tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "buy", sl_price, pos_size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "buy", tp_price, pos_size, tp_price, "tp")
    end

    bot.log("info", string.format("%s: SL=$%.2f (-%.1f%%), TP=$%.2f (+%.1f%%) → BB_mid=$%.2f",
        instance_name, sl_price, sl_pct, tp_price, tp_pct, bb_mid))
end

local function close_position(reason)
    if not in_position then return end
    bot.cancel_all_exchange(config.coin)
    sl_oid = nil
    tp_oid = nil

    local pos = bot.get_position(config.coin)
    if not pos or pos.size == 0 then
        in_position = false
        position_side = nil
        entry_price = 0
        bot.save_state("has_position", "false")
        return
    end

    local mid = bot.get_mid_price(config.coin)
    if not mid then return end

    local side = pos.size > 0 and "sell" or "buy"
    local size = math.abs(pos.size)
    local oid = bot.place_limit(config.coin, side, mid, size, { tif = "ioc", reduce_only = true })
    if not oid then
        local px = side == "sell" and mid * 0.99 or mid * 1.01
        bot.place_limit(config.coin, side, px, size, { tif = "ioc", reduce_only = true })
    end
    bot.log("info", string.format("%s: CLOSE — %s", instance_name, reason))

    in_position = false
    position_side = nil
    entry_price = 0
    bot.save_state("has_position", "false")
end

-- ── State for TP target ────────────────────────────────────────────────────
local entry_bb_mid = 0  -- BB middle at entry time, used for TP calculation

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format("%s: BB Sniper [RSI<%d/%d ADX<%d SL=%.1fxATR CD=%ds]",
        instance_name, config.rsi_oversold, config.rsi_overbought,
        config.adx_max, config.sl_atr_mult, config.cooldown_sec))

    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end
    local st = bot.load_state("trade_count")
    if st then trade_count = tonumber(st) or 0 end
    local sw = bot.load_state("win_count")
    if sw then win_count = tonumber(sw) or 0 end

    local had = bot.load_state("has_position")
    if had == "true" then
        local pos = bot.get_position(config.coin)
        if pos and pos.size ~= 0 then
            in_position = true
            entry_price = pos.entry_px
            position_side = pos.size > 0 and "long" or "short"
            entry_time = bot.time()
            bot.cancel_all_exchange(config.coin)
            local ind = bot.get_indicators(config.coin, "1h", 50, pos.entry_px)
            entry_bb_mid = (ind and ind.bb_middle) or pos.entry_px
            place_sl_tp(entry_price, position_side, math.abs(pos.size), entry_bb_mid)
            bot.log("info", string.format("%s: restored position %s @ $%.2f",
                instance_name, position_side, entry_price))
        else
            bot.save_state("has_position", "false")
        end
    end
    last_check = bot.time()
end

function on_tick(coin, mid_price)
    if coin ~= config.coin then return end
    if not config.enabled then return end

    local now = bot.time()
    if now - last_check < config.check_sec then return end
    last_check = now

    -- Max hold: force close
    if in_position and now - entry_time > config.max_hold_sec then
        close_position(string.format("max hold %ds", config.max_hold_sec))
        last_trade = now
        return
    end

    -- In position: just monitor
    if in_position then
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            in_position = false
            position_side = nil
            entry_price = 0
            bot.cancel_all_exchange(config.coin)
            sl_oid = nil
            tp_oid = nil
        end
        return
    end

    -- Pending entry guard
    if entry_oid then
        if now - entry_placed_at > ENTRY_TIMEOUT then
            bot.cancel(config.coin, entry_oid)
            entry_oid = nil
        end
        return
    end

    -- Cooldown
    if now - last_trade < config.cooldown_sec then return end

    -- Get indicators
    local ind = bot.get_indicators(config.coin, "1h", 50, mid_price)
    if not ind then return end

    local rsi      = ind.rsi
    local adx      = ind.adx
    local bb_upper = ind.bb_upper
    local bb_lower = ind.bb_lower
    local bb_mid   = ind.bb_middle
    local bb_width = ind.bb_width

    if not rsi or not adx or not bb_upper or not bb_lower or not bb_mid or not bb_width then
        return
    end

    -- Filter: no trending markets (MR doesn't work)
    if adx > config.adx_max then return end

    -- Filter: need minimum BB width
    if bb_width < config.min_bb_width then return end

    -- LONG: price below BB lower + extreme RSI
    if mid_price < bb_lower and rsi < config.rsi_oversold then
        -- Check TP distance is worth it
        local tp_dist_pct = ((bb_mid - mid_price) / mid_price) * 100
        if tp_dist_pct < config.tp_min_pct then return end

        bot.log("info", string.format(
            "%s: SNIPE LONG — RSI=%.0f price=$%.2f < BB_low=$%.2f → target BB_mid=$%.2f (+%.1f%%)",
            instance_name, rsi, mid_price, bb_lower, bb_mid, tp_dist_pct))
        entry_bb_mid = bb_mid
        local oid = place_entry("long", mid_price)
        if oid then
            last_trade = now
            entry_time = now
        end
        return
    end

    -- SHORT: price above BB upper + extreme RSI
    if mid_price > bb_upper and rsi > config.rsi_overbought then
        local tp_dist_pct = ((mid_price - bb_mid) / mid_price) * 100
        if tp_dist_pct < config.tp_min_pct then return end

        bot.log("info", string.format(
            "%s: SNIPE SHORT — RSI=%.0f price=$%.2f > BB_up=$%.2f → target BB_mid=$%.2f (+%.1f%%)",
            instance_name, rsi, mid_price, bb_upper, bb_mid, tp_dist_pct))
        entry_bb_mid = bb_mid
        local oid = place_entry("short", mid_price)
        if oid then
            last_trade = now
            entry_time = now
        end
        return
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    bot.log("info", string.format("%s: FILL %s %.5f @ $%.2f pnl=%.4f",
        instance_name, fill.side, fill.size, fill.price, fill.closed_pnl))

    -- Entry fill
    if fill.closed_pnl == 0 then
        if not in_position then
            in_position = true
            entry_price = fill.price
            position_side = fill.side == "buy" and "long" or "short"
            entry_time = bot.time()
            bot.save_state("has_position", "true")
        end
        entry_oid = nil
        entry_placed_at = 0
        bot.cancel_all_exchange(config.coin)
        sl_oid = nil
        tp_oid = nil
        local pos = bot.get_position(config.coin)
        if pos and pos.size ~= 0 then
            place_sl_tp(entry_price, position_side, math.abs(pos.size), entry_bb_mid)
        end
        return
    end

    -- Exit fill
    last_trade = bot.time()
    trade_count = trade_count + 1
    if fill.closed_pnl > 0 then
        win_count = win_count + 1
    end
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))

    bot.log("info", string.format("%s: EXIT pnl=%.4f (win rate: %d/%d = %.0f%%)",
        instance_name, fill.closed_pnl, win_count, trade_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0))

    bot.cancel_all_exchange(config.coin)
    sl_oid = nil
    tp_oid = nil
    in_position = false
    position_side = nil
    entry_price = 0
    bot.save_state("has_position", "false")
end

function on_timer()
    if entry_oid and not in_position then
        if bot.time() - entry_placed_at > ENTRY_TIMEOUT then
            bot.cancel(config.coin, entry_oid)
            entry_oid = nil
        end
    end
    if not in_position then return end
    local pos = bot.get_position(config.coin)
    if not pos or pos.size == 0 then
        in_position = false
        position_side = nil
        entry_price = 0
        bot.cancel_all_exchange(config.coin)
        sl_oid = nil
        tp_oid = nil
    end
end

function on_shutdown()
    bot.log("info", string.format("%s: shutdown — %d trades, %d wins (%.0f%%)",
        instance_name, trade_count, win_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0))
    bot.save_state("enabled", tostring(config.enabled))
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))
    bot.save_state("has_position", tostring(in_position))
end
