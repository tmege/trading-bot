--[[
  RSI Divergence Strategy — Generic Multi-Coin

  Divergence detection between price and RSI on 1h candles.
  The COIN global is injected by the C engine before loading.
  Each instance runs independently with its own state.

  Logic:
  - BULLISH DIVERGENCE: price makes lower low but RSI makes higher low
    over last 10-20 bars → hidden buying pressure → LONG
  - BEARISH DIVERGENCE: price makes higher high but RSI makes lower high
    over last 10-20 bars → hidden selling pressure → SHORT
  - Uses raw candle data (bot.get_candles) for pivot detection
  - Uses bot.get_indicators for current RSI with live price injection
  - SL: 1.5%, TP: 3%, Lever: 5x, Size: $40
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = COIN or "ETH",
    leverage      = 7,

    -- Divergence detection
    lookback      = 20,          -- candles to scan for divergences
    min_lookback  = 5,           -- minimum distance between pivots
    pivot_window  = 2,           -- bars on each side to confirm pivot

    -- Position sizing
    entry_size    = 40.0,        -- USD per trade (fallback/minimum)
    equity_pct    = 0.10,        -- compound: 10% of account per trade
    max_size      = 60.0,        -- hard cap

    -- Exit targets
    tp_pct        = 3.0,
    sl_pct        = 1.5,

    -- Timing
    check_sec     = 15,
    cooldown_sec  = 300,         -- 5 min cooldown (divergences are rarer signals)
    max_hold_sec  = 14400,       -- max hold 4h

    enabled       = true,
}

-- Instance name (e.g. "rsi_divergence_1h_eth")
local instance_name = "rsi_divergence_1h_" .. config.coin:lower()

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
local position_side  = nil
local entry_price    = 0
local entry_time     = 0
local sl_oid         = nil
local tp_oid         = nil
local entry_oid       = nil       -- pending entry order (prevents ALO spam)
local entry_placed_at = 0
local ENTRY_TIMEOUT   = 60        -- cancel unfilled ALO after 60s
local trade_count    = 0
local win_count      = 0

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

local function get_candle_field(candle, field)
    -- Backtest uses: open, high, low, close, volume, time
    -- Live uses: o, h, l, c, v, t
    if field == "close" then return candle.close or candle.c end
    if field == "low"   then return candle.low or candle.l end
    if field == "high"  then return candle.high or candle.h end
    if field == "open"  then return candle.open or candle.o end
    if field == "time"  then return candle.time or candle.t end
    return nil
end

--- Find local lows (swing lows) in a price series.
-- A swing low at index i means price[i] <= all prices in [i-window, i+window].
local function find_swing_lows(values, window)
    local lows = {}
    for i = window + 1, #values - window do
        local is_low = true
        for j = i - window, i + window do
            if j ~= i and values[j] < values[i] then
                is_low = false
                break
            end
        end
        if is_low then
            table.insert(lows, { idx = i, val = values[i] })
        end
    end
    return lows
end

--- Find local highs (swing highs) in a price series.
local function find_swing_highs(values, window)
    local highs = {}
    for i = window + 1, #values - window do
        local is_high = true
        for j = i - window, i + window do
            if j ~= i and values[j] > values[i] then
                is_high = false
                break
            end
        end
        if is_high then
            table.insert(highs, { idx = i, val = values[i] })
        end
    end
    return highs
end

--- Detect bullish divergence: price lower low + RSI higher low
local function detect_bullish_div(price_lows, rsi_lows)
    if #price_lows < 2 or #rsi_lows < 2 then return false end

    -- Compare the two most recent swing lows
    local p1 = price_lows[#price_lows - 1]
    local p2 = price_lows[#price_lows]

    -- Find matching RSI lows (closest index)
    local r1, r2
    for _, r in ipairs(rsi_lows) do
        if math.abs(r.idx - p1.idx) <= 2 then r1 = r end
        if math.abs(r.idx - p2.idx) <= 2 then r2 = r end
    end

    if not r1 or not r2 then return false end

    -- Bullish div: price lower low but RSI higher low
    if p2.val < p1.val and r2.val > r1.val then
        return true, p1, p2, r1, r2
    end

    return false
end

--- Detect bearish divergence: price higher high + RSI lower high
local function detect_bearish_div(price_highs, rsi_highs)
    if #price_highs < 2 or #rsi_highs < 2 then return false end

    local p1 = price_highs[#price_highs - 1]
    local p2 = price_highs[#price_highs]

    local r1, r2
    for _, r in ipairs(rsi_highs) do
        if math.abs(r.idx - p1.idx) <= 2 then r1 = r end
        if math.abs(r.idx - p2.idx) <= 2 then r2 = r end
    end

    if not r1 or not r2 then return false end

    -- Bearish div: price higher high but RSI lower high
    if p2.val > p1.val and r2.val < r1.val then
        return true, p1, p2, r1, r2
    end

    return false
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

--- Compute simple RSI from close prices (for candle-by-candle historical RSI)
local function compute_rsi_series(closes, period)
    local rsi_vals = {}
    if #closes < period + 1 then return rsi_vals end

    -- Initialize average gain/loss
    local avg_gain = 0
    local avg_loss = 0
    for i = 2, period + 1 do
        local delta = closes[i] - closes[i - 1]
        if delta > 0 then avg_gain = avg_gain + delta
        else avg_loss = avg_loss + math.abs(delta) end
    end
    avg_gain = avg_gain / period
    avg_loss = avg_loss / period

    -- First RSI value
    if avg_loss == 0 then
        rsi_vals[period + 1] = 100
    else
        local rs = avg_gain / avg_loss
        rsi_vals[period + 1] = 100 - (100 / (1 + rs))
    end

    -- Subsequent values using exponential smoothing
    for i = period + 2, #closes do
        local delta = closes[i] - closes[i - 1]
        local gain = delta > 0 and delta or 0
        local loss = delta < 0 and math.abs(delta) or 0

        avg_gain = (avg_gain * (period - 1) + gain) / period
        avg_loss = (avg_loss * (period - 1) + loss) / period

        if avg_loss == 0 then
            rsi_vals[i] = 100
        else
            local rs = avg_gain / avg_loss
            rsi_vals[i] = 100 - (100 / (1 + rs))
        end
    end

    return rsi_vals
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "%s: RSI Divergence 1h, lookback=%d, SL=%.1f%%, TP=%.1f%%",
        instance_name, config.lookback, config.sl_pct, config.tp_pct))

    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end

    local saved_trades = bot.load_state("trade_count")
    if saved_trades then trade_count = tonumber(saved_trades) or 0 end
    local saved_wins = bot.load_state("win_count")
    if saved_wins then win_count = tonumber(saved_wins) or 0 end

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

    -- Get candles for divergence analysis
    local candles = bot.get_candles(config.coin, "1h", config.lookback + 5)
    if not candles or #candles < config.lookback then
        bot.log("warn", string.format("%s: not enough candles (%d/%d)",
            instance_name, candles and #candles or 0, config.lookback))
        return
    end

    -- Build close price and RSI arrays from candles
    local closes = {}
    local lows = {}
    local highs = {}
    for i = 1, #candles do
        local c = get_candle_field(candles[i], "close")
        local l = get_candle_field(candles[i], "low")
        local h = get_candle_field(candles[i], "high")
        if c then closes[i] = c end
        if l then lows[i] = l end
        if h then highs[i] = h end
    end

    if #closes < config.lookback then
        bot.log("warn", instance_name .. ": could not extract close prices from candles")
        return
    end

    -- Compute RSI series from candle closes (14-period)
    local rsi_series = compute_rsi_series(closes, 14)

    -- We need RSI values for recent candles; build aligned arrays for pivot detection
    -- Use only the last N candles where we have both price and RSI
    local start_idx = 0
    for i = #closes, 1, -1 do
        if rsi_series[i] then
            start_idx = i
        end
    end
    if start_idx == 0 or (#closes - start_idx) < config.min_lookback then
        bot.log("debug", instance_name .. ": not enough RSI history for divergence scan")
        return
    end

    -- Extract aligned price lows/highs and RSI for the analysis window
    local price_low_vals = {}
    local price_high_vals = {}
    local rsi_vals = {}
    local count = 0
    for i = start_idx, #closes do
        count = count + 1
        price_low_vals[count] = lows[i] or closes[i]
        price_high_vals[count] = highs[i] or closes[i]
        rsi_vals[count] = rsi_series[i]
    end

    if count < config.min_lookback + 2 * config.pivot_window then
        return
    end

    -- Find swing pivots
    local pw = config.pivot_window
    local price_swing_lows = find_swing_lows(price_low_vals, pw)
    local price_swing_highs = find_swing_highs(price_high_vals, pw)
    local rsi_swing_lows = find_swing_lows(rsi_vals, pw)
    local rsi_swing_highs = find_swing_highs(rsi_vals, pw)

    bot.log("debug", string.format(
        "%s: candles=%d pivots: priceLows=%d priceHighs=%d rsiLows=%d rsiHighs=%d",
        instance_name, count, #price_swing_lows, #price_swing_highs,
        #rsi_swing_lows, #rsi_swing_highs))

    -- Check for bullish divergence → LONG
    local bull, bp1, bp2, br1, br2 = detect_bullish_div(price_swing_lows, rsi_swing_lows)
    if bull then
        -- Verify the most recent pivot is near the end (within last 3 bars)
        if bp2.idx >= count - 3 then
            bot.log("info", string.format(
                "%s: BULLISH DIVERGENCE — price low $%.2f→$%.2f (lower) but RSI %.1f→%.1f (higher) → LONG",
                instance_name, bp1.val, bp2.val, br1.val, br2.val))
            local oid = place_entry("long", mid_price)
            if oid then
                last_trade = now
                entry_time = now
            end
            return
        end
    end

    -- Check for bearish divergence → SHORT
    local bear, sp1, sp2, sr1, sr2 = detect_bearish_div(price_swing_highs, rsi_swing_highs)
    if bear then
        if sp2.idx >= count - 3 then
            bot.log("info", string.format(
                "%s: BEARISH DIVERGENCE — price high $%.2f→$%.2f (higher) but RSI %.1f→%.1f (lower) → SHORT",
                instance_name, sp1.val, sp2.val, sr1.val, sr2.val))
            local oid = place_entry("short", mid_price)
            if oid then
                last_trade = now
                entry_time = now
            end
            return
        end
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

    if in_position then
        bot.log("info", instance_name .. ": position open, SL/TP on exchange")
    end
end
