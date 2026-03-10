--[[
  Momentum / Trend Following Strategy — ETH  [PRIMARY]

  Best-performing strategy from backtesting:
  +486 USDC across 5 scenarios, Sharpe 12.60, wins 4/5 scenarios.

  Logic:
  - Golden cross (EMA12 > EMA26) + RSI > 50 + volume spike → LONG
  - Death cross (EMA12 < EMA26) + RSI < 50 + volume spike → SHORT
  - ATR-based trailing stop (2x initial, 1.5x trailing after +1.5%)
  - Two-step trailing: breakeven at +2%, lock +2% at +4%
  - Only 1 position at a time, ride the trend
  - MACD histogram for momentum confirmation
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = "ETH",
    leverage      = 5,

    -- Indicators
    ema_fast      = 12,
    ema_slow      = 26,
    rsi_period    = 14,
    atr_period    = 14,
    macd_signal   = 9,

    -- Entry conditions
    rsi_long      = 50,          -- RSI must be > this for long
    rsi_short     = 50,          -- RSI must be < this for short
    volume_mult   = 1.3,         -- volume > avg * mult for confirmation
    volume_window = 20,

    -- Position sizing
    entry_size    = 100.0,       -- USD per trade
    max_size      = 150.0,       -- hard cap

    -- ATR trailing stop
    atr_sl_mult   = 2.0,         -- initial SL = entry -/+ ATR * mult
    atr_trail_mult = 1.5,        -- trailing = peak -/+ ATR * trail_mult
    trail_activate = 1.5,        -- activate trailing at +1.5%

    -- Take profit
    tp_pct        = 10.0,        -- take profit at +10%

    -- Timing
    check_sec     = 60,          -- check every 60s
    cooldown_sec  = 1800,        -- 30 min between trades

    -- Re-entry
    allow_reentry = true,        -- re-enter if trend continues after exit

    enabled       = true,
}

-- ── State ──────────────────────────────────────────────────────────────────

local last_check     = 0
local last_trade     = 0
local in_position    = false
local position_side  = nil       -- "long" or "short"
local entry_price    = 0
local peak_price     = 0         -- for trailing stop
local sl_oid         = nil
local tp_oid         = nil
local current_sl     = 0         -- current stop level
local prev_ema_fast  = 0
local prev_ema_slow  = 0
local trend          = "none"    -- "up", "down", "none"

-- ── Helpers ────────────────────────────────────────────────────────────────

local function check_volume_spike()
    local candles = bot.get_candles(config.coin, "1h", config.volume_window + 5)
    if not candles or #candles < config.volume_window + 1 then
        return false, 0, 0
    end

    local sum = 0
    local start = #candles - config.volume_window
    if start < 1 then start = 1 end
    for i = start, #candles - 1 do
        sum = sum + candles[i].v
    end
    local avg = sum / config.volume_window
    local cur = candles[#candles].v

    return cur > avg * config.volume_mult, cur, avg
end

local function detect_crossover(ind)
    local ema_f = ind.ema_fast or ind.ema
    local ema_s = ind.ema_slow

    if not ema_f or not ema_s then return "none", false end

    local cross = "none"
    local is_cross = false

    -- Golden cross: fast crosses above slow
    if prev_ema_fast <= prev_ema_slow and ema_f > ema_s then
        cross = "golden"
        is_cross = true
    -- Death cross: fast crosses below slow
    elseif prev_ema_fast >= prev_ema_slow and ema_f < ema_s then
        cross = "death"
        is_cross = true
    end

    prev_ema_fast = ema_f
    prev_ema_slow = ema_s

    -- Track trend direction
    if ema_f > ema_s then
        trend = "up"
    elseif ema_f < ema_s then
        trend = "down"
    end

    return cross, is_cross
end

local function place_entry(side, mid)
    local size = config.entry_size / mid
    local oid = bot.place_limit(config.coin, side == "long" and "buy" or "sell",
                                mid, size, { tif = "ioc" })
    if oid then
        bot.log("info", string.format("momentum: ENTRY %s @ $%.2f ($%.0f)",
            string.upper(side), mid, config.entry_size))
        return oid
    end
    return nil
end

local function place_stops(fill_price, side, atr)
    local sl_price, tp_price

    if side == "long" then
        sl_price = fill_price - atr * config.atr_sl_mult
        tp_price = fill_price * (1 + config.tp_pct / 100)

        sl_oid = bot.place_trigger(config.coin, "sell", sl_price,
                                    config.entry_size / fill_price, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "sell", tp_price,
                                    config.entry_size / fill_price, tp_price, "tp")
    else
        sl_price = fill_price + atr * config.atr_sl_mult
        tp_price = fill_price * (1 - config.tp_pct / 100)

        sl_oid = bot.place_trigger(config.coin, "buy", sl_price,
                                    config.entry_size / fill_price, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "buy", tp_price,
                                    config.entry_size / fill_price, tp_price, "tp")
    end

    current_sl = sl_price
    bot.log("info", string.format("momentum: SL=$%.2f (ATR*%.1f), TP=$%.2f (+%.0f%%)",
        sl_price, config.atr_sl_mult, tp_price, config.tp_pct))
end

local function move_stop_loss(new_sl, reason)
    if sl_oid then bot.cancel(config.coin, sl_oid) end

    local sl_side = position_side == "long" and "sell" or "buy"
    local pos = bot.get_position(config.coin)
    local size = config.entry_size / entry_price
    if pos and pos.size ~= 0 then size = math.abs(pos.size) end

    sl_oid = bot.place_trigger(config.coin, sl_side, new_sl, size, new_sl, "sl")
    current_sl = new_sl

    bot.log("info", string.format("momentum: SL → $%.2f (%s)", new_sl, reason))
end

local trail_step = 0  -- 0=initial, 1=breakeven, 2=locked profit

local function update_trailing(mid, atr)
    if not in_position or entry_price <= 0 then return end

    local pnl_pct
    if position_side == "long" then
        if mid > peak_price then peak_price = mid end
        pnl_pct = ((mid - entry_price) / entry_price) * 100
    else
        if mid < peak_price then peak_price = mid end
        pnl_pct = ((entry_price - mid) / entry_price) * 100
    end

    -- Step 1: move SL to breakeven at +2%
    if trail_step == 0 and pnl_pct >= 2.0 then
        local be_sl = entry_price
        if (position_side == "long" and be_sl > current_sl) or
           (position_side == "short" and be_sl < current_sl) then
            move_stop_loss(be_sl, "breakeven at +2%")
            trail_step = 1
        end
    end

    -- Step 2: lock +2% profit at +4%
    if trail_step == 1 and pnl_pct >= 4.0 then
        local lock_sl
        if position_side == "long" then
            lock_sl = entry_price * 1.02
        else
            lock_sl = entry_price * 0.98
        end
        if (position_side == "long" and lock_sl > current_sl) or
           (position_side == "short" and lock_sl < current_sl) then
            move_stop_loss(lock_sl, "lock +2% at +4%")
            trail_step = 2
        end
    end

    -- ATR trailing after activation threshold
    if pnl_pct < config.trail_activate then return end

    local new_sl
    if position_side == "long" then
        new_sl = peak_price - atr * config.atr_trail_mult
        if new_sl <= current_sl then return end  -- only move up
    else
        new_sl = peak_price + atr * config.atr_trail_mult
        if new_sl >= current_sl then return end  -- only move down
    end

    move_stop_loss(new_sl, string.format("ATR trail peak=$%.2f +%.1f%%", peak_price, pnl_pct))
end

local function close_position(reason)
    bot.log("info", string.format("momentum: closing — %s", reason))

    if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
    if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end

    local pos = bot.get_position(config.coin)
    if pos and pos.size ~= 0 then
        local mid = bot.get_mid_price(config.coin)
        if mid then
            local side = pos.size > 0 and "sell" or "buy"
            bot.place_limit(config.coin, side, mid, math.abs(pos.size),
                           { tif = "ioc", reduce_only = true })
        end
    end

    in_position = false
    position_side = nil
    entry_price = 0
    peak_price = 0
    current_sl = 0
    trail_step = 0
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "momentum_eth: EMA%d/%d, RSI, ATR trail (%.1fx), TP=%.0f%%",
        config.ema_fast, config.ema_slow, config.atr_trail_mult, config.tp_pct))

    local saved_enabled = bot.load_state("enabled")
    if saved_enabled == "false" then config.enabled = false end

    -- Check existing position
    local pos = bot.get_position(config.coin)
    if pos and pos.size ~= 0 then
        in_position = true
        entry_price = pos.entry_px
        peak_price = pos.entry_px
        position_side = pos.size > 0 and "long" or "short"
        bot.log("info", string.format("momentum: existing %s %.5f @ $%.2f",
            position_side, pos.size, pos.entry_px))
    end

    -- Initialize EMA values
    local ind = bot.get_indicators(config.coin, "1h", 50)
    if ind then
        prev_ema_fast = ind.ema_fast or ind.ema or 0
        prev_ema_slow = ind.ema_slow or 0
    end

    last_check = bot.time()
end

function on_tick(coin, mid_price)
    if coin ~= config.coin then return end
    if not config.enabled then return end

    local now = bot.time()
    if now - last_check < config.check_sec then return end
    last_check = now

    local ind = bot.get_indicators(config.coin, "1h", 50)
    if not ind then return end

    local atr = ind.atr or 0

    -- Update trailing stop if in position
    if in_position then
        if atr > 0 then
            update_trailing(mid_price, atr)
        end

        -- Check for trend reversal while in position
        local cross, is_cross = detect_crossover(ind)
        if is_cross then
            if (position_side == "long" and cross == "death") or
               (position_side == "short" and cross == "golden") then
                close_position("trend reversal (" .. cross .. " cross)")
                last_trade = now
            end
        end

        -- Check position still exists
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            bot.log("info", "momentum: position closed (SL/TP)")
            in_position = false
            position_side = nil
            entry_price = 0
            peak_price = 0
            current_sl = 0
            trail_step = 0
            sl_oid = nil
            tp_oid = nil
        end

        return
    end

    -- Not in position: look for entry signals
    if now - last_trade < config.cooldown_sec then return end

    local cross, is_cross = detect_crossover(ind)
    local rsi = ind.rsi or 50

    -- Golden cross → LONG
    if is_cross and cross == "golden" and rsi > config.rsi_long then
        local has_vol = check_volume_spike()
        if has_vol then
            -- MACD confirmation (optional but helps)
            local macd_hist = ind.macd_histogram
            local macd_ok = not macd_hist or macd_hist > 0

            if macd_ok then
                bot.log("info", string.format(
                    "momentum: GOLDEN CROSS + RSI=%.0f + volume OK → LONG", rsi))
                local oid = place_entry("long", mid_price)
                if oid then
                    last_trade = now
                end
            end
        end
    end

    -- Death cross → SHORT
    if is_cross and cross == "death" and rsi < config.rsi_short then
        local has_vol = check_volume_spike()
        if has_vol then
            local macd_hist = ind.macd_histogram
            local macd_ok = not macd_hist or macd_hist < 0

            if macd_ok then
                bot.log("info", string.format(
                    "momentum: DEATH CROSS + RSI=%.0f + volume OK → SHORT", rsi))
                local oid = place_entry("short", mid_price)
                if oid then
                    last_trade = now
                end
            end
        end
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    bot.log("info", string.format("momentum: FILL %s %.5f @ $%.2f pnl=%.4f",
        fill.side, fill.size, fill.price, fill.closed_pnl))

    -- Entry fill
    if not in_position and fill.closed_pnl == 0 then
        in_position = true
        entry_price = fill.price
        peak_price = fill.price
        trail_step = 0
        position_side = fill.side == "buy" and "long" or "short"

        local ind = bot.get_indicators(config.coin, "1h", 50)
        local atr = (ind and ind.atr) or (fill.price * 0.02)  -- fallback 2%
        place_stops(fill.price, position_side, atr)
        return
    end

    -- SL/TP fill
    if fill.oid == sl_oid then
        bot.log("info", string.format("momentum: STOP LOSS — pnl=%.4f", fill.closed_pnl))
        if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end
        in_position = false
        position_side = nil
        entry_price = 0
        peak_price = 0
        current_sl = 0
        trail_step = 0
        sl_oid = nil
    end

    if fill.oid == tp_oid then
        bot.log("info", string.format("momentum: TAKE PROFIT — pnl=%.4f", fill.closed_pnl))
        if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
        in_position = false
        position_side = nil
        entry_price = 0
        peak_price = 0
        current_sl = 0
        trail_step = 0
        tp_oid = nil
    end
end

function on_timer()
    -- Periodic position sync
    if in_position then
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            in_position = false
            position_side = nil
            entry_price = 0
            peak_price = 0
            current_sl = 0
            trail_step = 0
            if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
            if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end
        end
    end
end

function on_advisory(json_str)
    bot.log("info", "momentum_eth: advisory: " .. json_str)

    local pause = json_str:match('"pause"%s*:%s*(true)')
    local resume = json_str:match('"pause"%s*:%s*(false)')

    if pause then
        config.enabled = false
        bot.save_state("enabled", "false")
        if in_position then close_position("advisory pause") end
        bot.log("warn", "momentum: PAUSED by advisory")
    end

    if resume and not config.enabled then
        config.enabled = true
        bot.save_state("enabled", "true")
        bot.log("info", "momentum: RESUMED by advisory")
    end
end

function on_shutdown()
    bot.log("info", "momentum_eth: shutdown")
    bot.save_state("enabled", tostring(config.enabled))
    if in_position then
        bot.log("info", string.format("momentum: %s position open, SL/TP on exchange",
            position_side))
    end
end
