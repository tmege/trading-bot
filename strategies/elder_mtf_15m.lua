--[[
  Elder Triple Screen Strategy — Generic Multi-Coin

  Multi-timeframe strategy inspired by Dr. Alexander Elder's Triple Screen system.
  Uses 15m as the primary timeframe with long MAs as higher-TF proxies.
  The COIN global is injected by the C engine before loading.
  Each instance runs independently with its own state.

  Three screens:
  - Screen 1 (4h trend proxy): ema_26 vs sma_200 for long-term trend direction
  - Screen 2 (1h pullback proxy): RSI extremes in the direction of the trend
  - Screen 3 (15m entry trigger): MACD histogram zero-line crossover

  LONG: trend up (ema_26 > sma_200) + pullback (rsi < 40) + macd_histogram crosses above 0
  SHORT: trend down (ema_26 < sma_200) + overbought (rsi > 60) + macd_histogram crosses below 0

  SL: 2%, TP: 4%, Lever: 5x, Size: $40
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = COIN or "ETH",
    leverage      = 10,

    -- Screen 1: Trend (4h proxy via 15m long MAs)
    -- ema_26 > sma_200 = bullish macro trend

    -- Screen 2: Pullback detection
    rsi_pullback_long  = 45,    -- RSI < 45 in uptrend = pullback buy zone
    rsi_pullback_short = 55,    -- RSI > 55 in downtrend = pullback sell zone

    -- Screen 3: MACD histogram direction (rising=long, falling=short)

    -- Position sizing
    entry_size    = 40.0,
    equity_pct    = 0.10,        -- compound: 10% of account per trade
    max_size      = 60.0,

    -- Exit targets
    sl_pct        = 2.0,
    tp_pct        = 4.0,

    -- Timing
    check_sec     = 5,          -- fast check for precise entry
    cooldown_sec  = 120,        -- 2 min between trades
    max_hold_sec  = 14400,      -- max hold 4h

    enabled       = true,
}

-- Instance name (e.g. "elder_mtf_15m_eth")
local instance_name = "elder_mtf_15m_" .. config.coin:lower()

local function get_trade_size()
    local acct = bot.get_account_value()
    if acct and acct > 0 then
        return acct * config.equity_pct
    end
    return config.entry_size
end

-- ── State ──────────────────────────────────────────────────────────────────

local last_check         = 0
local last_trade         = 0
local in_position        = false
local position_side      = nil       -- "long" or "short"
local entry_price        = 0
local entry_time         = 0
local sl_oid             = nil
local tp_oid             = nil
local entry_oid       = nil       -- pending entry order (prevents ALO spam)
local entry_placed_at = 0
local ENTRY_TIMEOUT   = 60        -- cancel unfilled ALO after 60s
local trade_count        = 0
local win_count          = 0
local prev_macd_hist     = nil       -- previous MACD histogram for zero-cross detection

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
    if not pos or pos.size == 0 then
        in_position = false
        position_side = nil
        entry_price = 0
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
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "%s: Elder Triple Screen — SL=%.1f%%, TP=%.1f%%, check=%ds, cooldown=%ds",
        instance_name, config.sl_pct, config.tp_pct, config.check_sec, config.cooldown_sec))

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

    -- Get indicators (15m timeframe, 200 candles for SMA200)
    local ind = bot.get_indicators(config.coin, "15m", 200, mid_price)
    if not ind then
        bot.log("warn", instance_name .. ": get_indicators returned nil")
        return
    end

    -- Screen 1: EMA26 vs SMA200 as higher-TF trend proxy
    local ema26     = ind.ema_26
    local sma200    = ind.sma_200
    local rsi       = ind.rsi
    local macd_hist = ind.macd_histogram

    if not ema26 or not sma200 or not rsi or not macd_hist then
        bot.log("warn", string.format("%s: missing fields — ema26=%s sma200=%s rsi=%s macd_h=%s",
            instance_name, tostring(ema26), tostring(sma200), tostring(rsi), tostring(macd_hist)))
        return
    end

    -- Determine screens: EMA26 > SMA200 = bullish macro trend
    local trend_bullish = ema26 > sma200
    local trend_bearish = ema26 < sma200

    -- Detect MACD histogram direction (zero-cross OR rising/falling)
    local macd_rising = false
    local macd_falling = false
    local macd_cross_up = false
    local macd_cross_down = false
    if prev_macd_hist ~= nil then
        macd_cross_up   = prev_macd_hist <= 0 and macd_hist > 0
        macd_cross_down = prev_macd_hist >= 0 and macd_hist < 0
        macd_rising     = macd_hist > prev_macd_hist
        macd_falling    = macd_hist < prev_macd_hist
    end
    prev_macd_hist = macd_hist

    bot.log("debug", string.format(
        "%s: %s=$%.2f EMA26=%.2f SMA200=%.2f RSI=%.1f MACD_H=%.4f trend=%s rising=%s falling=%s",
        instance_name, config.coin, mid_price, ema26, sma200, rsi, macd_hist,
        trend_bullish and "BULL" or (trend_bearish and "BEAR" or "FLAT"),
        tostring(macd_rising), tostring(macd_falling)))

    -- ── In-position management ──

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

        -- Optional: close if trend reverses strongly
        if position_side == "long" and trend_bearish and macd_cross_down then
            close_position("trend reversal — EMA26 < SMA200 + MACD cross down")
            last_trade = now
            return
        end
        if position_side == "short" and trend_bullish and macd_cross_up then
            close_position("trend reversal — EMA26 > SMA200 + MACD cross up")
            last_trade = now
            return
        end

        return
    end

    -- ── No position: Triple Screen entry check ──

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

    -- LONG: Screen 1 (bullish trend) + Screen 2 (pullback) + Screen 3 (MACD rising or cross up)
    if trend_bullish and rsi < config.rsi_pullback_long and (macd_cross_up or (macd_rising and macd_hist > 0)) then
        bot.log("info", string.format(
            "%s: TRIPLE SCREEN LONG — EMA26>SMA200, RSI=%.0f (pullback), MACD %s",
            instance_name, rsi, macd_cross_up and "cross up" or "rising"))
        local oid = place_entry("long", mid_price)
        if oid then
            last_trade = now
            entry_time = now
        end
        return
    end

    -- SHORT: Screen 1 (bearish trend) + Screen 2 (overbought) + Screen 3 (MACD falling or cross down)
    if trend_bearish and rsi > config.rsi_pullback_short and (macd_cross_down or (macd_falling and macd_hist < 0)) then
        bot.log("info", string.format(
            "%s: TRIPLE SCREEN SHORT — EMA26<SMA200, RSI=%.0f (overbought), MACD %s",
            instance_name, rsi, macd_cross_down and "cross down" or "falling"))
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
