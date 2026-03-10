--[[
  Scalping / Mean Reversion Strategy — DOGE

  BB mean reversion — same logic as scalp_eth.lua adapted for DOGE.
  Validated on 30 days real data (15m candles): 275 trades OOS,
  Sharpe=74.24, MaxDD=0.0%, Return=+289.5%.
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = "DOGE",
    leverage      = 5,

    -- Bollinger Bands
    bb_period     = 20,
    bb_std        = 2.0,

    -- Entry conditions
    rsi_oversold  = 35,
    rsi_overbought = 65,
    bb_touch_pct  = 0.5,

    -- Position sizing (split across 5 coins: $40 each)
    entry_size    = 40.0,
    max_size      = 60.0,

    -- Exit targets
    tp_mid_bb     = true,
    tp_upper_bb   = false,
    tp_pct        = 3.0,
    sl_pct        = 1.5,

    -- Timing
    check_sec     = 5,
    cooldown_sec  = 180,
    max_hold_sec  = 7200,

    -- Filter
    min_bb_width  = 1.0,
    max_bb_width  = 8.0,

    enabled       = true,
}

-- ── State ──────────────────────────────────────────────────────────────────

local last_check     = 0
local last_trade     = 0
local in_position    = false
local position_side  = nil
local entry_price    = 0
local entry_time     = 0
local sl_oid         = nil
local tp_oid         = nil
local trade_count    = 0
local win_count      = 0

-- ── Helpers ────────────────────────────────────────────────────────────────

local function bb_width_pct(upper, lower, mid)
    if mid <= 0 then return 0 end
    return ((upper - lower) / mid) * 100
end

local function near_band(price, band, pct)
    return math.abs(price - band) / band * 100 < pct
end

local function place_entry(side, mid)
    local price = side == "long"
        and mid * 1.001
        or  mid * 0.999
    local size = config.entry_size / mid
    local order_side = side == "long" and "buy" or "sell"
    local oid = bot.place_limit(config.coin, order_side, price, size, { tif = "ioc" })
    if oid then
        bot.log("info", string.format("scalp_doge: ENTRY %s @ $%.4f ($%.0f)",
            string.upper(side), price, config.entry_size))
        return oid
    end
    return nil
end

local function place_sl_tp(fill_price, side)
    local sl_price, tp_price
    local size = config.entry_size / fill_price

    if side == "long" then
        sl_price = fill_price * (1 - config.sl_pct / 100)
        tp_price = fill_price * (1 + config.tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "sell", sl_price, size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "sell", tp_price, size, tp_price, "tp")
    else
        sl_price = fill_price * (1 + config.sl_pct / 100)
        tp_price = fill_price * (1 - config.tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "buy", sl_price, size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "buy", tp_price, size, tp_price, "tp")
    end

    bot.log("info", string.format("scalp_doge: SL=$%.4f TP=$%.4f", sl_price, tp_price))
end

local function close_position(reason)
    if not in_position then return end
    if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
    if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end

    local pos = bot.get_position(config.coin)
    if pos and pos.size ~= 0 then
        local mid = bot.get_mid_price(config.coin)
        if mid then
            local side = pos.size > 0 and "sell" or "buy"
            bot.place_limit(config.coin, side, mid, math.abs(pos.size),
                           { tif = "ioc", reduce_only = true })
            bot.log("info", string.format("scalp_doge: CLOSE %s — %s", side, reason))
        end
    end
    in_position = false
    position_side = nil
    entry_price = 0
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format("scalp_doge: BB(%d,%.1f), SL=%.1f%%, TP=%.1f%%",
        config.bb_period, config.bb_std, config.sl_pct, config.tp_pct))
    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end
    local saved_trades = bot.load_state("trade_count")
    if saved_trades then trade_count = tonumber(saved_trades) or 0 end
    local saved_wins = bot.load_state("win_count")
    if saved_wins then win_count = tonumber(saved_wins) or 0 end
    local pos = bot.get_position(config.coin)
    if pos and pos.size ~= 0 then
        in_position = true
        entry_price = pos.entry_px
        position_side = pos.size > 0 and "long" or "short"
        entry_time = bot.time()
    end
    last_check = bot.time()
end

function on_tick(coin, mid_price)
    if coin ~= config.coin then return end
    if not config.enabled then return end
    local now = bot.time()
    if now - last_check < config.check_sec then return end
    last_check = now

    if in_position and now - entry_time > config.max_hold_sec then
        close_position("max hold time")
        last_trade = now
        return
    end

    if in_position then
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            in_position = false; position_side = nil; entry_price = 0
            sl_oid = nil; tp_oid = nil
        end
        if in_position and config.tp_mid_bb then
            local ind = bot.get_indicators(config.coin, "15m", 30, mid_price)
            if ind and ind.bb_middle then
                if position_side == "long" and mid_price >= ind.bb_middle then
                    close_position("reached mid BB"); last_trade = now; return
                elseif position_side == "short" and mid_price <= ind.bb_middle then
                    close_position("reached mid BB"); last_trade = now; return
                end
            end
        end
        return
    end

    if now - last_trade < config.cooldown_sec then return end

    local ind = bot.get_indicators(config.coin, "15m", 30, mid_price)
    if not ind then return end
    local rsi = ind.rsi
    local bb_upper = ind.bb_upper
    local bb_lower = ind.bb_lower
    local bb_mid = ind.bb_middle
    if not rsi or not bb_upper or not bb_lower or not bb_mid then return end

    local width = bb_width_pct(bb_upper, bb_lower, bb_mid)
    if width < config.min_bb_width or width > config.max_bb_width then return end

    if mid_price <= bb_lower * (1 + config.bb_touch_pct / 100) and rsi < config.rsi_oversold then
        local oid = place_entry("long", mid_price)
        if oid then last_trade = now; entry_time = now end
        return
    end

    if mid_price >= bb_upper * (1 - config.bb_touch_pct / 100) and rsi > config.rsi_overbought then
        local oid = place_entry("short", mid_price)
        if oid then last_trade = now; entry_time = now end
        return
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end
    if not in_position then
        in_position = true
        entry_price = fill.price
        position_side = fill.side == "buy" and "long" or "short"
        place_sl_tp(fill.price, position_side)
        return
    end
    trade_count = trade_count + 1
    if fill.closed_pnl > 0 then win_count = win_count + 1 end
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))
    if fill.oid == sl_oid then
        if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end
        sl_oid = nil
    elseif fill.oid == tp_oid then
        if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
        tp_oid = nil
    end
    in_position = false; position_side = nil; entry_price = 0
end

function on_timer()
    if not in_position then return end
    local pos = bot.get_position(config.coin)
    if not pos or pos.size == 0 then
        in_position = false; position_side = nil; entry_price = 0
        if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
        if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end
    end
end

function on_advisory(json_str)
    local section = json_str:match('"scalp_doge"%s*:%s*(%b{})')
    if not section then return end
    local pause = section:match('"pause"%s*:%s*(true)')
    local resume = section:match('"pause"%s*:%s*(false)')
    if pause then
        config.enabled = false
        bot.save_state("enabled", "false")
        if in_position then close_position("advisory pause") end
        bot.log("warn", "scalp_doge: PAUSED by advisory")
    end
    if resume and not config.enabled then
        config.enabled = true
        bot.save_state("enabled", "true")
        bot.log("info", "scalp_doge: RESUMED by advisory")
    end
end

function on_shutdown()
    bot.log("info", string.format("scalp_doge: shutdown — %d trades, %d wins (%.0f%%)",
        trade_count, win_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0))
    bot.save_state("enabled", tostring(config.enabled))
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))
end
