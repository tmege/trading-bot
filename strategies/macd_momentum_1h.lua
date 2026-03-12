--[[
  MACD Momentum Strategy — Generic Multi-Coin

  Momentum strategy based on MACD histogram direction + RSI filter.
  The COIN global is injected by the C engine before loading.
  Each instance runs independently with its own state.

  Logic:
  - LONG: MACD histogram > 0 AND rising (current > previous) AND RSI < 65
  - SHORT: MACD histogram < 0 AND falling (current < previous) AND RSI > 35
  - Cooldown: 600s (10 candles on 1h) between trades
  - Trailing stop: 1.5x ATR (dynamic, adjusts with volatility)
  - TP: 3x ATR (let winners run in momentum moves)
  - Lever: 5x, Size: $40
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = COIN or "ETH",
    leverage      = 7,

    -- Entry conditions (RSI guard against overbought/oversold)
    rsi_long_max  = 65,          -- LONG only if RSI < 65 (not overbought)
    rsi_short_min = 35,          -- SHORT only if RSI > 35 (not oversold)

    -- Position sizing
    entry_size    = 40.0,        -- USD per trade (fallback/minimum)
    equity_pct    = 0.10,        -- 10% of account per trade
    max_size      = 60.0,        -- hard cap

    -- Exit targets (ATR-based, set dynamically at entry)
    sl_atr_mult   = 1.5,        -- trailing stop distance = 1.5x ATR
    tp_atr_mult   = 3.0,        -- take profit distance = 3x ATR

    -- Timing
    check_sec     = 10,
    cooldown_sec  = 600,         -- 10 min (10 candles on 1h) between trades
    max_hold_sec  = 14400,       -- max hold 4h for 1h momentum

    enabled       = true,
}

-- Instance name (e.g. "macd_momentum_1h_eth")
local instance_name = "macd_momentum_1h_" .. config.coin:lower()

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

-- MACD histogram tracking
local prev_histogram = nil
-- Trailing stop state
local trail_distance = 0         -- ATR-based distance set at entry
local best_price     = 0         -- best price since entry (for trailing)

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

local function place_sl_tp_atr(fill_price, side, atr, pos_size)
    local sl_dist = atr * config.sl_atr_mult
    local tp_dist = atr * config.tp_atr_mult
    local sl_price, tp_price
    local size = pos_size

    if side == "long" then
        sl_price = fill_price - sl_dist
        tp_price = fill_price + tp_dist

        sl_oid = bot.place_trigger(config.coin, "sell", sl_price, size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "sell", tp_price, size, tp_price, "tp")
    else
        sl_price = fill_price + sl_dist
        tp_price = fill_price - tp_dist

        sl_oid = bot.place_trigger(config.coin, "buy", sl_price, size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "buy", tp_price, size, tp_price, "tp")
    end

    trail_distance = sl_dist
    best_price = fill_price

    bot.log("info", string.format("%s: SL=$%.2f (%.1f ATR), TP=$%.2f (%.1f ATR), ATR=$%.2f",
        instance_name, sl_price, config.sl_atr_mult, tp_price, config.tp_atr_mult, atr))
end

local function update_trailing_stop(mid_price)
    if not in_position or trail_distance <= 0 then return end

    local should_update = false
    local new_sl_price

    if position_side == "long" and mid_price > best_price then
        best_price = mid_price
        new_sl_price = best_price - trail_distance
        should_update = true
    elseif position_side == "short" and mid_price < best_price then
        best_price = mid_price
        new_sl_price = best_price + trail_distance
        should_update = true
    end

    if should_update and new_sl_price then
        -- Cancel old SL and place new one
        if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
        local pos = bot.get_position(config.coin)
        if not pos then return end
        local size = math.abs(pos.size)
        local sl_side = position_side == "long" and "sell" or "buy"
        sl_oid = bot.place_trigger(config.coin, sl_side, new_sl_price, size, new_sl_price, "sl")
        bot.log("debug", string.format("%s: TRAIL SL updated to $%.2f (best=$%.2f)",
            instance_name, new_sl_price, best_price))
    end
end

local function close_position(reason)
    if not in_position then return end

    if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
    if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end

    local pos = bot.get_position(config.coin)
    if not pos or pos.size == 0 then
        in_position = false
        position_side = nil
        entry_price = 0
        trail_distance = 0
        best_price = 0
        return
    end

    local mid = bot.get_mid_price(config.coin)
    if not mid then return end

    local side = pos.size > 0 and "sell" or "buy"
    local size = math.abs(pos.size)

    local oid = bot.place_limit(config.coin, side, mid, size,
                                { tif = "ioc", reduce_only = true })
    if not oid then
        local aggressive_px = side == "sell" and mid * 0.99 or mid * 1.01
        oid = bot.place_limit(config.coin, side, aggressive_px, size,
                              { tif = "ioc", reduce_only = true })
        if oid then
            bot.log("warn", string.format("%s: CLOSE %s (retry slippage) — %s",
                instance_name, side, reason))
        else
            bot.log("error", string.format("%s: CLOSE FAILED — %s, retry next tick",
                instance_name, reason))
            return
        end
    else
        bot.log("info", string.format("%s: CLOSE %s — %s", instance_name, side, reason))
    end

    in_position = false
    position_side = nil
    entry_price = 0
    trail_distance = 0
    best_price = 0
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "%s: MACD Momentum 1h, SL=%.1fxATR, TP=%.1fxATR, RSI long<%d short>%d, cooldown=%ds",
        instance_name, config.sl_atr_mult, config.tp_atr_mult,
        config.rsi_long_max, config.rsi_short_min, config.cooldown_sec))

    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end

    local saved_trades = bot.load_state("trade_count")
    if saved_trades then trade_count = tonumber(saved_trades) or 0 end
    local saved_wins = bot.load_state("win_count")
    if saved_wins then win_count = tonumber(saved_wins) or 0 end

    local saved_histo = bot.load_state("prev_histogram")
    if saved_histo then prev_histogram = tonumber(saved_histo) end

    local saved_trail = bot.load_state("trail_distance")
    if saved_trail then trail_distance = tonumber(saved_trail) or 0 end
    local saved_best = bot.load_state("best_price")
    if saved_best then best_price = tonumber(saved_best) or 0 end

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

    -- Update trailing stop if in position
    if in_position then
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            in_position = false
            position_side = nil
            entry_price = 0
            sl_oid = nil
            tp_oid = nil
            trail_distance = 0
            best_price = 0
        else
            update_trailing_stop(mid_price)
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

    -- Get 1h indicators (50 candles: MACD needs 26+9=35 minimum)
    local ind = bot.get_indicators(config.coin, "1h", 50, mid_price)
    if not ind then
        bot.log("warn", instance_name .. ": get_indicators returned nil")
        return
    end

    local rsi = ind.rsi
    local macd_histo = ind.macd_histogram
    local atr = ind.atr

    if not rsi or not macd_histo or not atr then
        bot.log("warn", string.format("%s: missing indicator fields (rsi=%s histo=%s atr=%s)",
            instance_name, tostring(rsi), tostring(macd_histo), tostring(atr)))
        return
    end

    bot.log("debug", string.format(
        "%s: %s=$%.2f MACD_H=%.4f prev=%.4f RSI=%.1f ATR=%.2f",
        instance_name, config.coin, mid_price, macd_histo,
        prev_histogram or 0, rsi, atr))

    -- Detect histogram momentum (direction change)
    if prev_histogram then
        local histo_rising  = macd_histo > prev_histogram
        local histo_falling = macd_histo < prev_histogram

        -- LONG: histogram > 0 AND rising AND RSI not overbought
        if not in_position and macd_histo > 0 and histo_rising and rsi < config.rsi_long_max then
            bot.log("info", string.format(
                "%s: MACD MOMENTUM LONG — histo=%.4f rising (prev=%.4f) RSI=%.1f ATR=%.2f",
                instance_name, macd_histo, prev_histogram, rsi, atr))
            local oid = place_entry("long", mid_price)
            if oid then
                last_trade = now
                entry_time = now
                bot.save_state("entry_atr", tostring(atr))
            end
        end

        -- SHORT: histogram < 0 AND falling AND RSI not oversold
        if not in_position and macd_histo < 0 and histo_falling and rsi > config.rsi_short_min then
            bot.log("info", string.format(
                "%s: MACD MOMENTUM SHORT — histo=%.4f falling (prev=%.4f) RSI=%.1f ATR=%.2f",
                instance_name, macd_histo, prev_histogram, rsi, atr))
            local oid = place_entry("short", mid_price)
            if oid then
                last_trade = now
                entry_time = now
                bot.save_state("entry_atr", tostring(atr))
            end
        end
    end

    prev_histogram = macd_histo
    bot.save_state("prev_histogram", tostring(macd_histo))
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

        -- Retrieve ATR saved at entry signal
        local entry_atr = tonumber(bot.load_state("entry_atr") or "0")
        if entry_atr <= 0 then
            -- Fallback: use 1.5% of price as default distance
            entry_atr = fill.price * 0.015
            bot.log("warn", string.format("%s: no entry_atr, using fallback %.2f", instance_name, entry_atr))
        end
        place_sl_tp_atr(fill.price, position_side, entry_atr, fill.size)
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
    trail_distance = 0
    best_price = 0
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
        trail_distance = 0
        best_price = 0
    else
        -- Save trailing state periodically
        bot.save_state("trail_distance", tostring(trail_distance))
        bot.save_state("best_price", tostring(best_price))
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
    bot.save_state("trail_distance", tostring(trail_distance))
    bot.save_state("best_price", tostring(best_price))

    if in_position then
        bot.log("info", instance_name .. ": position open, SL/TP on exchange")
    end
end
