--[[
  Triple Confirmation Mean Reversion Strategy — Generic Multi-Coin

  Mean reversion with triple filter: BB + RSI + MACD histogram direction.
  The COIN global is injected by the C engine before loading.
  Each instance runs independently with its own state.

  Logic:
  - LONG: price at/below lower BB + RSI < 30 + MACD histogram rising → oversold bounce
  - SHORT: price at/above upper BB + RSI > 70 + MACD histogram falling → overbought rejection
  - BB width filter: 1.5% - 6% (skip if too tight or too wide/trending)
  - Triple filter eliminates false signals common in single-indicator strategies
  - SL: 1.2%, TP: 2.5%, Lever: 5x
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = COIN or "ETH",
    leverage      = 10,

    -- Entry conditions (loosened from 30/70 to increase signal frequency)
    rsi_oversold  = 38,
    rsi_overbought = 62,
    bb_touch_pct  = 1.0,         -- price within 1% of BB = "touching"

    -- Position sizing
    entry_size    = 40.0,        -- USD per trade (fallback/minimum)
    equity_pct    = 0.10,        -- compound: 10% of account per trade
    max_size      = 60.0,        -- hard cap

    -- Exit targets
    tp_pct        = 2.5,
    sl_pct        = 1.2,

    -- Timing
    check_sec     = 5,
    cooldown_sec  = 60,
    max_hold_sec  = 3600,        -- max hold 1h

    -- BB width filter (widened to capture more setups)
    min_bb_width  = 0.8,
    max_bb_width  = 10.0,

    enabled       = true,
}

-- Instance name (e.g. "triple_confirm_15m_eth")
local instance_name = "triple_confirm_15m_" .. config.coin:lower()

local function get_trade_size()
    local acct = bot.get_account_value()
    if acct and acct > 0 then
        return acct * config.equity_pct
    end
    return config.entry_size
end

-- ── State ──────────────────────────────────────────────────────────────────

local last_check     = 0
local last_trade     = 0
local in_position    = false
local position_side  = nil       -- "long" or "short"
local entry_price    = 0
local entry_time     = 0
local sl_oid         = nil
local tp_oid         = nil
local entry_oid       = nil       -- pending entry order (prevents ALO spam)
local entry_placed_at = 0
local ENTRY_TIMEOUT   = 60        -- cancel unfilled ALO after 60s
local trade_count    = 0
local win_count      = 0
local prev_histogram = nil       -- track MACD histogram for direction

-- ── Crash protection ──────────────────────────────────────────────────────
local price_history     = {}      -- {price, time} ring buffer
local PRICE_HIST_SIZE   = 30      -- track last 30 price samples
local VELOCITY_THRESH   = 10.0    -- skip entry if >10% move in window
local losing_streak     = 0
local MAX_LOSING_STREAK = 3       -- pause 5 min after 3 consecutive losses
local streak_pause_until = 0

local function velocity_check(mid_price, now)
    table.insert(price_history, {p = mid_price, t = now})
    if #price_history > PRICE_HIST_SIZE then
        table.remove(price_history, 1)
    end
    if #price_history < 5 then return true end
    local min_p, max_p = mid_price, mid_price
    for _, v in ipairs(price_history) do
        if v.p < min_p then min_p = v.p end
        if v.p > max_p then max_p = v.p end
    end
    local move_pct = ((max_p - min_p) / min_p) * 100
    if move_pct >= VELOCITY_THRESH then
        bot.log("warn", string.format("%s: VELOCITY GUARD — %.1f%% move detected, skipping entry",
            instance_name, move_pct))
        return false
    end
    return true
end

-- ── Helpers ────────────────────────────────────────────────────────────────

local function bb_width_pct(upper, lower, mid)
    if mid <= 0 then return 0 end
    return ((upper - lower) / mid) * 100
end

local function place_entry(side, mid)
    -- Cancel any existing pending entry first
    if entry_oid then
        bot.cancel(config.coin, entry_oid)
        entry_oid = nil
    end

    -- ALO post-only: price inside spread for maker fee
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

local function place_sl_tp(fill_price, side, pos_size)
    local sl_price, tp_price
    local size = pos_size

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

    bot.log("info", string.format("%s: SL=$%.2f (%.1f%%), TP=$%.2f (+%.1f%%)",
        instance_name, sl_price, config.sl_pct, tp_price, config.tp_pct))
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
            bot.log("info", string.format("%s: CLOSE %s — %s", instance_name, side, reason))
        end
    end

    in_position = false
    position_side = nil
    entry_price = 0
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "%s: Triple Confirm RSI<%d/>%d, SL=%.1f%%, TP=%.1f%%, BB width [%.1f-%.1f%%]",
        instance_name, config.rsi_oversold, config.rsi_overbought,
        config.sl_pct, config.tp_pct, config.min_bb_width, config.max_bb_width))

    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end

    local saved_trades = bot.load_state("trade_count")
    if saved_trades then trade_count = tonumber(saved_trades) or 0 end
    local saved_wins = bot.load_state("win_count")
    if saved_wins then win_count = tonumber(saved_wins) or 0 end

    local saved_histo = bot.load_state("prev_histogram")
    if saved_histo then prev_histogram = tonumber(saved_histo) end

    -- Check existing position
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

    -- Force close if held too long
    if in_position and now - entry_time > config.max_hold_sec then
        close_position(string.format("max hold time (%ds)", config.max_hold_sec))
        last_trade = now
        return
    end

    -- Check if position still exists
    if in_position then
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            in_position = false
            position_side = nil
            entry_price = 0
            sl_oid = nil
            tp_oid = nil
        end
        return
    end

    -- Pending entry order guard — only 1 ALO at a time
    if entry_oid then
        if now - entry_placed_at > ENTRY_TIMEOUT then
            bot.cancel(config.coin, entry_oid)
            bot.log("info", string.format("%s: entry order timeout (%ds), cancelled", instance_name, ENTRY_TIMEOUT))
            entry_oid = nil
            entry_placed_at = 0
        end
        return
    end

    -- Cooldown
    if now - last_trade < config.cooldown_sec then return end

    -- Losing streak pause (5 min cooldown after MAX_LOSING_STREAK consecutive losses)
    if losing_streak >= MAX_LOSING_STREAK and now < streak_pause_until then
        return
    elseif losing_streak >= MAX_LOSING_STREAK then
        losing_streak = 0
    end

    -- Velocity guard (skip entry if price moved too fast)
    if not velocity_check(mid_price, now) then return end

    -- Get indicators
    local ind = bot.get_indicators(config.coin, "15m", 50, mid_price)
    if not ind then
        bot.log("warn", instance_name .. ": get_indicators returned nil")
        return
    end

    local rsi = ind.rsi
    local bb_upper = ind.bb_upper
    local bb_lower = ind.bb_lower
    local bb_mid = ind.bb_middle
    local macd_histo = ind.macd_histogram

    if not rsi or not bb_upper or not bb_lower or not bb_mid or not macd_histo then
        bot.log("warn", string.format("%s: missing indicator fields", instance_name))
        return
    end

    -- Track histogram direction
    local histo_rising = false
    local histo_falling = false
    if prev_histogram then
        histo_rising = macd_histo > prev_histogram
        histo_falling = macd_histo < prev_histogram
    end
    prev_histogram = macd_histo
    bot.save_state("prev_histogram", tostring(macd_histo))

    -- BB width filter
    local width = bb_width_pct(bb_upper, bb_lower, bb_mid)
    bot.log("debug", string.format(
        "%s: %s=$%.2f BB=[%.2f/%.2f/%.2f] w=%.1f%% RSI=%.1f MACD_H=%.4f rising=%s falling=%s",
        instance_name, config.coin, mid_price, bb_lower, bb_mid, bb_upper,
        width, rsi, macd_histo, tostring(histo_rising), tostring(histo_falling)))

    if width < config.min_bb_width then
        bot.log("debug", string.format("%s: BB too tight (%.1f%%), skipping", instance_name, width))
        return
    end
    if width > config.max_bb_width then
        bot.log("debug", string.format("%s: BB too wide (%.1f%%), trending", instance_name, width))
        return
    end

    -- LONG: lower BB touch + RSI oversold + MACD histogram not deeply negative (momentum ok)
    if mid_price <= bb_lower * (1 + config.bb_touch_pct / 100)
       and rsi < config.rsi_oversold
       and (histo_rising or macd_histo > -0.001) then
        bot.log("info", string.format(
            "%s: TRIPLE CONFIRM LONG — BB lower ($%.2f<=$%.2f) + RSI=%.0f + MACD_H rising (%.4f)",
            instance_name, mid_price, bb_lower, rsi, macd_histo))
        local oid = place_entry("long", mid_price)
        if oid then
            last_trade = now
            entry_time = now
        end
        return
    end

    -- SHORT: upper BB touch + RSI overbought + MACD histogram not deeply positive
    if mid_price >= bb_upper * (1 - config.bb_touch_pct / 100)
       and rsi > config.rsi_overbought
       and (histo_falling or macd_histo < 0.001) then
        bot.log("info", string.format(
            "%s: TRIPLE CONFIRM SHORT — BB upper ($%.2f>=$%.2f) + RSI=%.0f + MACD_H falling (%.4f)",
            instance_name, mid_price, bb_upper, rsi, macd_histo))
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

    bot.log("info", string.format("%s: FILL %s %.5f @ $%.2f pnl=%.4f fee=%.4f",
        instance_name, fill.side, fill.size, fill.price, fill.closed_pnl, fill.fee))

    -- Entry fill
    if not in_position then
        entry_oid = nil
        entry_placed_at = 0
        in_position = true
        entry_price = fill.price
        position_side = fill.side == "buy" and "long" or "short"
        place_sl_tp(fill.price, position_side, fill.size)
        return
    end

    -- Exit fill
    trade_count = trade_count + 1
    if fill.closed_pnl > 0 then
        win_count = win_count + 1
        losing_streak = 0
    else
        losing_streak = losing_streak + 1
        if losing_streak >= MAX_LOSING_STREAK then
            streak_pause_until = bot.time() + 300
            bot.log("warn", string.format("%s: %d consecutive losses — pausing 5 min",
                instance_name, losing_streak))
        end
    end

    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))

    if fill.oid == sl_oid then
        bot.log("info", string.format("%s: SL hit — pnl=%.4f (win rate: %d/%d = %.0f%%)",
            instance_name, fill.closed_pnl, win_count, trade_count,
            trade_count > 0 and (win_count / trade_count * 100) or 0))
        if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end
        sl_oid = nil
    elseif fill.oid == tp_oid then
        bot.log("info", string.format("%s: TP hit — pnl=%.4f (win rate: %d/%d = %.0f%%)",
            instance_name, fill.closed_pnl, win_count, trade_count,
            trade_count > 0 and (win_count / trade_count * 100) or 0))
        if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
        tp_oid = nil
    end

    in_position = false
    position_side = nil
    entry_price = 0
end

function on_timer()
    -- Cancel stale entry orders
    if entry_oid and not in_position then
        if bot.time() - entry_placed_at > ENTRY_TIMEOUT then
            bot.cancel(config.coin, entry_oid)
            entry_oid = nil
            entry_placed_at = 0
        end
    end

    if not in_position then return end

    local pos = bot.get_position(config.coin)
    if not pos or pos.size == 0 then
        in_position = false
        position_side = nil
        entry_price = 0
        if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
        if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end
    end
end

function on_advisory(json_str)
    bot.log("info", instance_name .. ": advisory: " .. json_str)

    local section = json_str:match('"' .. instance_name .. '"%s*:%s*(%b{})')
    if not section then return end

    local pause = section:match('"pause"%s*:%s*(true)')
    local resume = section:match('"pause"%s*:%s*(false)')

    if pause then
        config.enabled = false
        bot.save_state("enabled", "false")
        if in_position then close_position("advisory pause") end
        bot.log("warn", instance_name .. ": PAUSED by advisory")
    end

    if resume and not config.enabled then
        config.enabled = true
        bot.save_state("enabled", "true")
        bot.log("info", instance_name .. ": RESUMED by advisory")
    end
end

function on_shutdown()
    bot.log("info", string.format("%s: shutdown — %d trades, %d wins (%.0f%%)",
        instance_name, trade_count, win_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0))
    bot.save_state("enabled", tostring(config.enabled))
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))
    bot.save_state("prev_histogram", tostring(prev_histogram or 0))

    if in_position then
        bot.log("info", instance_name .. ": position open, SL/TP on exchange")
    end
end
