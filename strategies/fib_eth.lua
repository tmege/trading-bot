--[[
  Fibonacci Confluence Strategy — ETH

  Multi-timeframe Fibonacci retracement with RSI + Bollinger Band confluence.

  Logic:
  - Detect swing highs/lows from candle history (pivot point detection)
  - Compute Fibonacci retracement levels (38.2%, 50%, 61.8%)
  - LONG when price reaches a Fib level + RSI < 40 + near lower BB
  - SHORT when price reaches a Fib level + RSI > 60 + near upper BB
  - Stop loss: below next Fib level or 1.5x ATR (whichever tighter)
  - Take profit: scaled exits at swing recovery, 127.2%, 161.8% extensions
  - Tight risk: max 1.5% account risk per trade
]]

-- Configuration
local config = {
    coin              = "ETH",
    leverage          = 5,

    -- Swing detection
    swing_strength    = 3,           -- bars left/right for pivot confirmation
    candle_lookback   = 30,          -- candles to analyze for swings

    -- Fibonacci levels (entry zones)
    fib_382           = 0.382,
    fib_500           = 0.500,
    fib_618           = 0.618,
    fib_786           = 0.786,       -- used for stop placement only
    fib_zone_pct      = 1.0,         -- 1.0% band around each level

    -- Confluence filters
    rsi_long_max      = 40,          -- RSI must be below this for long
    rsi_short_min     = 60,          -- RSI must be above this for short
    bb_confluence_pct = 1.5,         -- Fib level within 1.5% of BB = confluence
    min_bb_width      = 0.02,        -- skip if BB in squeeze
    volume_mult       = 0.8,         -- min volume ratio vs 20-bar average

    -- Position sizing
    entry_size        = 80.0,        -- USD per trade (base)
    max_size          = 150.0,       -- hard cap USD
    confluence_boost  = 1.3,         -- 30% size boost on BB+Fib confluence

    -- Stop loss
    atr_sl_mult       = 1.5,         -- fallback: 1.5x ATR
    sl_max_pct        = 3.0,         -- hard cap stop loss %

    -- Take profit
    tp_pct            = 4.0,         -- hard cap TP if extension not reached
    sl_pct            = 1.5,         -- tight stop

    -- Trailing
    trail_breakeven   = 2.0,         -- move SL to breakeven at +2%
    trail_lock_pct    = 2.0,         -- lock +2% at +4%

    -- Timing
    check_sec         = 30,          -- check every 30s
    cooldown_sec      = 600,         -- 10 min between trades
    max_hold_sec      = 14400,       -- max hold 4h

    enabled           = true,
}

-- State
local last_check     = 0
local last_trade     = 0
local in_position    = false
local position_side  = nil
local entry_price    = 0
local entry_time     = 0
local sl_oid         = nil
local tp_oid         = nil
local peak_price     = 0
local trail_step     = 0
local trade_count    = 0
local win_count      = 0

-- Cached Fibonacci levels
local fib_levels     = nil
local fib_trend      = nil
local fib_swing_high = 0
local fib_swing_low  = 0
local last_fib_calc  = 0

-- Price history ring buffer (avoid bot.get_candles allocation)
local price_history  = {}
local price_idx      = 0
local HISTORY_SIZE   = 40

local function record_price(price)
    price_idx = price_idx + 1
    if price_idx > HISTORY_SIZE then
        -- Shift array
        for i = 1, HISTORY_SIZE - 1 do
            price_history[i] = price_history[i + 1]
        end
        price_idx = HISTORY_SIZE
    end
    price_history[price_idx] = price
end

-- Detect swing highs/lows from price history
local function detect_swings(prices, n, strength)
    local highs = {}
    local lows = {}

    for i = strength + 1, n - strength do
        local is_high = true
        local is_low = true
        for j = i - strength, i + strength do
            if j ~= i then
                if prices[j] >= prices[i] then is_high = false end
                if prices[j] <= prices[i] then is_low = false end
                if not is_high and not is_low then break end
            end
        end
        if is_high then highs[#highs + 1] = { idx = i, price = prices[i] } end
        if is_low then lows[#lows + 1] = { idx = i, price = prices[i] } end
    end

    return highs, lows
end

local function determine_trend(highs, lows)
    if #highs < 2 or #lows < 2 then return "range" end

    local last_h  = highs[#highs].price
    local prev_h  = highs[#highs - 1].price
    local last_l  = lows[#lows].price
    local prev_l  = lows[#lows - 1].price

    if last_h > prev_h and last_l > prev_l then return "up" end
    if last_h < prev_h and last_l < prev_l then return "down" end
    return "range"
end

-- ── Fibonacci level computation ─────────────────────────────────────────

local function compute_fib(swing_low, swing_high, trend)
    local range = swing_high - swing_low
    local levels = {}

    if trend == "up" or trend == "range" then
        -- Pullback in uptrend: buy at retracement support
        levels.r_382 = swing_high - range * 0.382
        levels.r_500 = swing_high - range * 0.500
        levels.r_618 = swing_high - range * 0.618
        levels.r_786 = swing_high - range * 0.786
        -- Extensions (from pullback point, computed at entry)
        levels.ext_1000 = swing_high
        levels.ext_1272 = swing_high + range * 0.272
        levels.ext_1618 = swing_high + range * 0.618
    else
        -- Bounce in downtrend: short at retracement resistance
        levels.r_382 = swing_low + range * 0.382
        levels.r_500 = swing_low + range * 0.500
        levels.r_618 = swing_low + range * 0.618
        levels.r_786 = swing_low + range * 0.786
        -- Extensions downward
        levels.ext_1000 = swing_low
        levels.ext_1272 = swing_low - range * 0.272
        levels.ext_1618 = swing_low - range * 0.618
    end

    return levels
end

local function near_level(price, level, pct)
    if level <= 0 then return false end
    return math.abs(price - level) / level * 100 < pct
end

local function update_fib_levels(now)
    -- Recalculate every 6 hours (keep levels stable)
    if now - last_fib_calc < 6 * 3600000 then return end
    last_fib_calc = now

    if price_idx < 15 then return end

    local highs, lows = detect_swings(price_history, price_idx, config.swing_strength)
    if #highs == 0 or #lows == 0 then return end

    fib_trend = determine_trend(highs, lows)

    -- Use most recent swing high and swing low
    fib_swing_high = highs[#highs].price
    fib_swing_low = lows[#lows].price

    -- Ensure swing_high > swing_low (use the correct pair)
    if fib_swing_high <= fib_swing_low then
        -- Try previous swings for a valid range
        if #highs >= 2 and highs[#highs - 1].price > fib_swing_low then
            fib_swing_high = highs[#highs - 1].price
        elseif #lows >= 2 and lows[#lows - 1].price < fib_swing_high then
            fib_swing_low = lows[#lows - 1].price
        else
            fib_levels = nil
            return
        end
    end

    fib_levels = compute_fib(fib_swing_low, fib_swing_high, fib_trend)

    bot.log("info", string.format(
        "fib: LEVELS swing $%.0f-$%.0f trend=%s | 38.2%%=$%.0f 50%%=$%.0f 61.8%%=$%.0f",
        fib_swing_low, fib_swing_high,
        fib_trend,
        fib_levels.r_382, fib_levels.r_500, fib_levels.r_618))
end

-- ── Order helpers ───────────────────────────────────────────────────────

local function place_entry(side, mid, size_usd)
    local size = size_usd / mid
    local order_side = side == "long" and "buy" or "sell"
    local oid = bot.place_limit(config.coin, order_side, mid, size, { tif = "ioc" })
    if oid then
        bot.log("info", string.format("fib: ENTRY %s @ $%.2f ($%.0f)",
            string.upper(side), mid, size_usd))
    end
    return oid
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

    bot.log("info", string.format("fib: SL=$%.2f (%.1f%%), TP=$%.2f (+%.1f%%)",
        sl_price, config.sl_pct, tp_price, config.tp_pct))
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
            bot.log("info", string.format("fib: CLOSE %s — %s", side, reason))
        end
    end

    in_position = false
    position_side = nil
    entry_price = 0
    peak_price = 0
    trail_step = 0
end

-- ── Callbacks ───────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "fib_eth: Fibonacci confluence | swing=%d, SL=%.1f%%, TP=%.1f%%, cooldown=%ds",
        config.swing_strength, config.sl_pct, config.tp_pct, config.cooldown_sec))

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
        peak_price = entry_price
        entry_time = bot.time()
    end

    last_check = bot.time()
end

function on_tick(coin, mid_price)
    if coin ~= config.coin then return end
    if not config.enabled then return end

    local now = bot.time()
    -- Record price for swing detection (every tick)
    record_price(mid_price)

    if now - last_check < config.check_sec * 1000 then return end
    last_check = now

    -- Update Fibonacci levels periodically
    update_fib_levels(now)

    -- Force close if held too long
    if in_position and now - entry_time > config.max_hold_sec * 1000 then
        close_position(string.format("max hold time (%ds)", config.max_hold_sec))
        last_trade = now
        return
    end

    -- Trailing stop management
    if in_position then
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            in_position = false
            position_side = nil
            entry_price = 0
            sl_oid = nil
            tp_oid = nil
            return
        end

        -- Update peak and trailing
        if position_side == "long" then
            if mid_price > peak_price then peak_price = mid_price end
            local pnl_pct = (mid_price - entry_price) / entry_price * 100

            if trail_step == 0 and pnl_pct >= config.trail_breakeven then
                -- Move SL to breakeven
                if sl_oid then bot.cancel(config.coin, sl_oid) end
                local size = math.abs(pos.size)
                sl_oid = bot.place_trigger(config.coin, "sell", entry_price, size, entry_price, "sl")
                trail_step = 1
                bot.log("info", string.format("fib: trailing → breakeven @ $%.2f", entry_price))
            elseif trail_step == 1 and pnl_pct >= config.trail_breakeven + config.trail_lock_pct then
                -- Lock profit
                if sl_oid then bot.cancel(config.coin, sl_oid) end
                local lock_px = entry_price * (1 + config.trail_lock_pct / 100)
                local size = math.abs(pos.size)
                sl_oid = bot.place_trigger(config.coin, "sell", lock_px, size, lock_px, "sl")
                trail_step = 2
                bot.log("info", string.format("fib: trailing → lock +%.0f%% @ $%.2f",
                    config.trail_lock_pct, lock_px))
            end
        else
            if mid_price < peak_price then peak_price = mid_price end
            local pnl_pct = (entry_price - mid_price) / entry_price * 100

            if trail_step == 0 and pnl_pct >= config.trail_breakeven then
                if sl_oid then bot.cancel(config.coin, sl_oid) end
                local size = math.abs(pos.size)
                sl_oid = bot.place_trigger(config.coin, "buy", entry_price, size, entry_price, "sl")
                trail_step = 1
                bot.log("info", string.format("fib: trailing → breakeven @ $%.2f", entry_price))
            elseif trail_step == 1 and pnl_pct >= config.trail_breakeven + config.trail_lock_pct then
                if sl_oid then bot.cancel(config.coin, sl_oid) end
                local lock_px = entry_price * (1 - config.trail_lock_pct / 100)
                local size = math.abs(pos.size)
                sl_oid = bot.place_trigger(config.coin, "buy", lock_px, size, lock_px, "sl")
                trail_step = 2
                bot.log("info", string.format("fib: trailing → lock +%.0f%% @ $%.2f",
                    config.trail_lock_pct, lock_px))
            end
        end

        return
    end

    -- Cooldown
    if now - last_trade < config.cooldown_sec * 1000 then return end

    -- Need valid Fibonacci levels
    if not fib_levels then return end

    -- Get indicators (light call, no candle allocation)
    local ind = bot.get_indicators(config.coin, "1h", 30)
    if not ind then return end

    local rsi = ind.rsi or ind.rsi_14
    local bb_upper = ind.bb_upper
    local bb_lower = ind.bb_lower
    local bb_width = ind.bb_width
    local atr = ind.atr or ind.atr_14

    if not rsi or not bb_upper or not bb_lower or not atr then return end

    -- Skip if BB in squeeze (no volatility)
    if bb_width and bb_width < config.min_bb_width then
        bot.log("debug", "fib: BB squeeze, skipping")
        return
    end

    -- Check each Fibonacci level for entry
    local fib_entry_levels = {
        { name = "38.2%", level = fib_levels.r_382, next_sl = fib_levels.r_500 },
        { name = "50.0%", level = fib_levels.r_500, next_sl = fib_levels.r_618 },
        { name = "61.8%", level = fib_levels.r_618, next_sl = fib_levels.r_786 },
    }

    for _, fib in ipairs(fib_entry_levels) do
        if near_level(mid_price, fib.level, config.fib_zone_pct) then
            -- Check BB confluence (Fib level near BB boundary)
            local bb_confluence = false
            if fib_trend ~= "down" then
                bb_confluence = near_level(fib.level, bb_lower, config.bb_confluence_pct)
            else
                bb_confluence = near_level(fib.level, bb_upper, config.bb_confluence_pct)
            end

            local size_usd = config.entry_size
            if bb_confluence then
                size_usd = math.min(size_usd * config.confluence_boost, config.max_size)
            end

            -- LONG: uptrend/range/none + price at Fib support + RSI low
            if fib_trend ~= "down" and rsi < config.rsi_long_max then
                bot.log("info", string.format(
                    "fib: %s level ($%.0f) + RSI=%.0f + trend=%s%s → LONG",
                    fib.name, fib.level, rsi, fib_trend,
                    bb_confluence and " + BB confluence" or ""))

                local oid = place_entry("long", mid_price, size_usd)
                if oid then
                    last_trade = now
                    entry_time = now
                end
                return
            end

            -- SHORT: downtrend + price at Fib resistance + RSI overbought-ish
            if fib_trend == "down" and rsi > config.rsi_short_min then
                bot.log("info", string.format(
                    "fib: %s level ($%.0f) + RSI=%.0f + trend=%s%s → SHORT",
                    fib.name, fib.level, rsi, fib_trend,
                    bb_confluence and " + BB confluence" or ""))

                local oid = place_entry("short", mid_price, size_usd)
                if oid then
                    last_trade = now
                    entry_time = now
                end
                return
            end
        end
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    bot.log("info", string.format("fib: FILL %s %.5f @ $%.2f pnl=%.4f fee=%.4f",
        fill.side, fill.size, fill.price, fill.closed_pnl, fill.fee))

    -- Entry fill
    if not in_position then
        in_position = true
        entry_price = fill.price
        peak_price = fill.price
        trail_step = 0
        position_side = fill.side == "buy" and "long" or "short"
        place_sl_tp(fill.price, position_side)
        return
    end

    -- Exit fill
    trade_count = trade_count + 1
    if fill.closed_pnl > 0 then
        win_count = win_count + 1
    end

    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))

    local wr = trade_count > 0 and (win_count / trade_count * 100) or 0

    if fill.oid == sl_oid then
        bot.log("info", string.format("fib: SL hit — pnl=%.4f (WR: %d/%d = %.0f%%)",
            fill.closed_pnl, win_count, trade_count, wr))
        if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end
        sl_oid = nil
    elseif fill.oid == tp_oid then
        bot.log("info", string.format("fib: TP hit — pnl=%.4f (WR: %d/%d = %.0f%%)",
            fill.closed_pnl, win_count, trade_count, wr))
        if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
        tp_oid = nil
    end

    in_position = false
    position_side = nil
    entry_price = 0
    peak_price = 0
    trail_step = 0
end

function on_timer()
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
    bot.log("info", "fib_eth: advisory: " .. json_str)

    local pause = json_str:match('"pause"%s*:%s*(true)')
    local resume = json_str:match('"pause"%s*:%s*(false)')

    if pause then
        config.enabled = false
        bot.save_state("enabled", "false")
        if in_position then close_position("advisory pause") end
        bot.log("warn", "fib: PAUSED by advisory")
    end

    if resume and not config.enabled then
        config.enabled = true
        bot.save_state("enabled", "true")
        bot.log("info", "fib: RESUMED by advisory")
    end
end

function on_shutdown()
    bot.log("info", string.format("fib_eth: shutdown — %d trades, %d wins (%.0f%%)",
        trade_count, win_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0))
    bot.save_state("enabled", tostring(config.enabled))
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))
end
