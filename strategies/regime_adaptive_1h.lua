--[[
  Regime-Adaptive Strategy — Generic Multi-Coin

  Detects market regime via BB width + ADX and switches between
  mean-reversion mode (low vol) and trend-following mode (high vol).
  The COIN global is injected by the C engine before loading.
  Each instance runs independently with its own state.

  Regimes:
  - Low volatility (bb_width < 3% and adx < 20): Mean reversion (BB touch scalp)
  - High volatility (bb_width > 5% or adx > 25): Trend following (EMA cross)
  - Transition: close position when regime changes

  Mean reversion entries:
  - LONG: price < bb_lower + RSI < 35
  - SHORT: price > bb_upper + RSI > 65
  - SL: 1.2%, TP: 2%

  Trend entries:
  - LONG: ema_12 > ema_26 + adx > 25
  - SHORT: ema_12 < ema_26 + adx > 25
  - SL: 2%, TP: 4%
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = COIN or "ETH",
    -- leverage is set at engine/exchange level (config max_leverage)

    -- Regime detection with hysteresis (dead zone avoids oscillation)
    adx_mr_threshold  = 22,     -- below this → mean reversion
    adx_tr_threshold  = 28,     -- above this → trend
    -- Between 22-28: keep current regime (no whipsaw)

    -- Mean reversion params (low vol regime)
    mr_rsi_oversold   = 40,
    mr_rsi_overbought = 60,
    mr_sl_atr_mult    = 1.0,    -- SL = 1.0x ATR (MR: tight)
    mr_tp_atr_mult    = 1.5,    -- TP = 1.5x ATR
    mr_sl_pct_min     = 0.8,    -- floor
    mr_sl_pct_max     = 2.0,    -- cap
    mr_tp_pct_min     = 1.0,    -- floor

    -- Trend params (high vol regime)
    tr_adx_min        = 25,     -- minimum ADX for trend entry (standard "trend present")
    tr_rsi_max_long   = 70,     -- skip trend LONG if already overbought
    tr_rsi_min_short  = 30,     -- skip trend SHORT if already oversold
    tr_sl_atr_mult    = 1.5,    -- SL = 1.5x ATR (trend: wider)
    tr_tp_atr_mult    = 3.0,    -- TP = 3.0x ATR
    tr_sl_pct_min     = 1.2,    -- floor
    tr_sl_pct_max     = 3.5,    -- cap
    tr_tp_pct_min     = 2.0,    -- floor

    -- Position sizing
    entry_size    = 40.0,
    equity_pct    = 0.10,        -- 10% of account per trade
    max_size      = 80.0,

    -- Timing
    check_sec     = 60,          -- 1h TF: no need for faster than 60s
    cooldown_sec  = 120,
    max_hold_sec  = 14400,      -- max hold 4h

    enabled       = true,
}

-- Instance name (e.g. "regime_adaptive_1h_eth")
local instance_name = "regime_adaptive_1h_" .. config.coin:lower()

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
local current_regime = "unknown" -- "mean_reversion", "trend", "unknown"
local entry_regime   = nil       -- regime when position was opened
local entry_atr      = 0         -- ATR at signal time, for dynamic SL/TP

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

local function detect_regime(bb_width, adx)
    -- ADX-based regime with hysteresis to prevent oscillation at boundary
    if adx < config.adx_mr_threshold then
        return "mean_reversion"
    elseif adx > config.adx_tr_threshold then
        return "trend"
    else
        -- Dead zone (22-28): keep current regime to avoid whipsaw
        if current_regime == "unknown" then
            return "mean_reversion"  -- default for first detection
        end
        return current_regime
    end
end

local function place_entry(side, mid)
    -- Skip if opposing position exists on this coin (from another strategy)
    local existing = bot.get_position(config.coin)
    if existing and existing.size ~= 0 then
        local ex_side = existing.size > 0 and "long" or "short"
        if ex_side ~= side then
            bot.log("info", string.format("%s: SKIP — opposing %s position on %s",
                instance_name, ex_side, config.coin))
            return nil
        end
    end

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
        bot.log("info", string.format("%s: ENTRY %s @ $%.2f ($%.0f) [%s mode]",
            instance_name, string.upper(side), price, trade_usd, current_regime))
        return oid
    end
    return nil
end

local function place_sl_tp(fill_price, side, regime, pos_size)
    local is_mr = regime == "mean_reversion"
    local sl_mult   = is_mr and config.mr_sl_atr_mult or config.tr_sl_atr_mult
    local tp_mult   = is_mr and config.mr_tp_atr_mult or config.tr_tp_atr_mult
    local sl_min    = is_mr and config.mr_sl_pct_min  or config.tr_sl_pct_min
    local sl_max    = is_mr and config.mr_sl_pct_max  or config.tr_sl_pct_max
    local tp_min    = is_mr and config.mr_tp_pct_min  or config.tr_tp_pct_min

    -- ATR-based SL/TP, clamped to min/max
    local atr_val = entry_atr > 0 and entry_atr or (fill_price * 0.015)  -- fallback 1.5%
    local sl_pct = (sl_mult * atr_val / fill_price) * 100
    sl_pct = math.max(sl_min, math.min(sl_max, sl_pct))

    local tp_pct = (tp_mult * atr_val / fill_price) * 100
    tp_pct = math.max(tp_min, math.max(sl_pct * 1.2, tp_pct))  -- R:R >= 1.2

    local sl_price, tp_price
    local size = pos_size

    if side == "long" then
        sl_price = fill_price * (1 - sl_pct / 100)
        tp_price = fill_price * (1 + tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "sell", sl_price, size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "sell", tp_price, size, tp_price, "tp")
    else
        sl_price = fill_price * (1 + sl_pct / 100)
        tp_price = fill_price * (1 - tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "buy", sl_price, size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "buy", tp_price, size, tp_price, "tp")
    end

    bot.log("info", string.format("%s: SL=$%.2f (%.1f%% ATR), TP=$%.2f (+%.1f%%) R:R=1:%.1f [%s]",
        instance_name, sl_price, sl_pct, tp_price, tp_pct, tp_pct / sl_pct, regime))
end

local function close_position(reason)
    if not in_position then return end

    -- Cancel ALL orders on exchange for this coin (catches orphaned trigger children)
    bot.cancel_all_exchange(config.coin)
    sl_oid = nil
    tp_oid = nil

    local pos = bot.get_position(config.coin)
    if not pos or pos.size == 0 then
        in_position = false
        position_side = nil
        entry_price = 0
        entry_regime = nil
        bot.save_state("has_position", "false")
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
    entry_regime = nil
    bot.save_state("has_position", "false")
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "%s: Adaptive regime [MR: SL=%.1fx/TP=%.1fx ATR | TR: SL=%.1fx/TP=%.1fx ATR], check=%ds",
        instance_name, config.mr_sl_atr_mult, config.mr_tp_atr_mult,
        config.tr_sl_atr_mult, config.tr_tp_atr_mult, config.check_sec))

    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end

    local saved_trades = bot.load_state("trade_count")
    if saved_trades then trade_count = tonumber(saved_trades) or 0 end
    local saved_wins = bot.load_state("win_count")
    if saved_wins then win_count = tonumber(saved_wins) or 0 end

    local saved_regime = bot.load_state("entry_regime")
    if saved_regime then entry_regime = saved_regime end

    local saved_streak = bot.load_state("losing_streak")
    if saved_streak then losing_streak = tonumber(saved_streak) or 0 end
    local saved_pause = bot.load_state("streak_pause_until")
    if saved_pause then streak_pause_until = tonumber(saved_pause) or 0 end

    -- Restore position only if THIS strategy opened it (prevents cross-strategy claiming)
    local had_position = bot.load_state("has_position")
    if had_position == "true" then
        local pos = bot.get_position(config.coin)
        if pos and pos.size ~= 0 then
            in_position = true
            entry_price = pos.entry_px
            position_side = pos.size > 0 and "long" or "short"
            entry_time = bot.time()
            bot.log("info", string.format("%s: restored OWN position %s %.4f @ $%.2f",
                instance_name, position_side, math.abs(pos.size), entry_price))
            -- Clean stale orders on exchange before re-placing SL/TP
            bot.cancel_all_exchange(config.coin)
            local ind = bot.get_indicators(config.coin, "1h", 50, pos.entry_px)
            if ind and ind.atr and ind.atr > 0 then entry_atr = ind.atr end
            local restore_regime = bot.load_state("entry_regime") or "mean_reversion"
            entry_regime = restore_regime
            place_sl_tp(entry_price, position_side, restore_regime, math.abs(pos.size))
            bot.log("info", string.format("%s: re-placed SL/TP after restart [%s]", instance_name, restore_regime))
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

    -- Force close if held too long
    if in_position and now - entry_time > config.max_hold_sec then
        close_position(string.format("max hold time (%ds)", config.max_hold_sec))
        last_trade = now
        return
    end

    -- Get indicators (1h timeframe, 50 candles — MACD needs slow+signal-1=34 minimum)
    local ind = bot.get_indicators(config.coin, "1h", 50, mid_price)
    if not ind then
        bot.log("warn", instance_name .. ": get_indicators returned nil")
        return
    end

    local bb_width  = ind.bb_width
    local adx       = ind.adx
    local rsi       = ind.rsi
    local bb_upper  = ind.bb_upper
    local bb_lower  = ind.bb_lower
    local bb_mid    = ind.bb_middle
    local ema12     = ind.ema_12
    local ema26     = ind.ema_26

    local macd_hist = ind.macd_histogram
    local atr       = ind.atr

    if not bb_width or not adx or not rsi or not bb_upper or not bb_lower
       or not ema12 or not ema26 or not macd_hist or not atr then
        bot.log("warn", string.format("%s: missing indicator fields", instance_name))
        return
    end

    -- Detect current regime
    local new_regime = detect_regime(bb_width, adx)
    if new_regime ~= current_regime then
        bot.log("info", string.format("%s: regime change %s → %s (bb_w=%.4f adx=%.1f)",
            instance_name, current_regime, new_regime, bb_width, adx))
        current_regime = new_regime
    end

    bot.log("debug", string.format("%s: %s=$%.2f regime=%s bb_w=%.4f adx=%.1f rsi=%.1f ema12=%.2f ema26=%.2f",
        instance_name, config.coin, mid_price, current_regime, bb_width, adx, rsi, ema12, ema26))

    -- ── In-position management ──

    if in_position then
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            in_position = false
            position_side = nil
            entry_price = 0
            bot.cancel_all_exchange(config.coin)
            sl_oid = nil
            tp_oid = nil
            entry_regime = nil
            return
        end

        -- Smart regime-change exit: close if profitable or held > 30min
        if entry_regime and current_regime ~= entry_regime then
            local unrealized = 0
            if position_side == "long" then
                unrealized = (mid_price - entry_price) / entry_price * 100
            else
                unrealized = (entry_price - mid_price) / entry_price * 100
            end
            local hold_time = now - entry_time

            if unrealized > 0 then
                close_position(string.format("regime %s→%s (profit +%.2f%%)",
                    entry_regime, current_regime, unrealized))
                last_trade = now
                return
            elseif hold_time > 1800 then  -- 30 min
                close_position(string.format("regime %s→%s (held %ds, loss %.2f%%)",
                    entry_regime, current_regime, hold_time, unrealized))
                last_trade = now
                return
            else
                bot.log("info", string.format(
                    "%s: regime changed but holding (pnl=%.2f%%, held=%ds — letting SL/TP handle)",
                    instance_name, unrealized, hold_time))
            end
        end

        return
    end

    -- ── No position: check entry conditions ──

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

    -- Store ATR for dynamic SL/TP on fill
    entry_atr = atr

    if current_regime == "mean_reversion" then
        -- Mean reversion: BB touch + RSI extreme
        -- LONG: price < bb_lower + RSI < 35
        if mid_price < bb_lower and rsi < config.mr_rsi_oversold then
            bot.log("info", string.format(
                "%s: MR LONG — price $%.2f < BB_low $%.2f, RSI=%.0f",
                instance_name, mid_price, bb_lower, rsi))
            local oid = place_entry("long", mid_price)
            if oid then
                last_trade = now
                entry_time = now
                entry_regime = "mean_reversion"
                bot.save_state("entry_regime", entry_regime)
            end
            return
        end

        -- SHORT: price > bb_upper + RSI > 65
        if mid_price > bb_upper and rsi > config.mr_rsi_overbought then
            bot.log("info", string.format(
                "%s: MR SHORT — price $%.2f > BB_up $%.2f, RSI=%.0f",
                instance_name, mid_price, bb_upper, rsi))
            local oid = place_entry("short", mid_price)
            if oid then
                last_trade = now
                entry_time = now
                entry_regime = "mean_reversion"
                bot.save_state("entry_regime", entry_regime)
            end
            return
        end

    elseif current_regime == "trend" then
        -- Trend following: EMA12 vs EMA26 cross + ADX confirmation
        -- LONG: ema_12 > ema_26 + adx > threshold + MACD confirms + RSI not overbought
        if ema12 > ema26 and adx > config.tr_adx_min
           and macd_hist > 0 and rsi < config.tr_rsi_max_long then
            bot.log("info", string.format(
                "%s: TREND LONG — EMA12 $%.2f > EMA26 $%.2f, ADX=%.1f",
                instance_name, ema12, ema26, adx))
            local oid = place_entry("long", mid_price)
            if oid then
                last_trade = now
                entry_time = now
                entry_regime = "trend"
                bot.save_state("entry_regime", entry_regime)
            end
            return
        end

        -- SHORT: ema_12 < ema_26 + adx > threshold + MACD confirms + RSI not oversold
        if ema12 < ema26 and adx > config.tr_adx_min
           and macd_hist < 0 and rsi > config.tr_rsi_min_short then
            bot.log("info", string.format(
                "%s: TREND SHORT — EMA12 $%.2f < EMA26 $%.2f, ADX=%.1f",
                instance_name, ema12, ema26, adx))
            local oid = place_entry("short", mid_price)
            if oid then
                last_trade = now
                entry_time = now
                entry_regime = "trend"
                bot.save_state("entry_regime", entry_regime)
            end
            return
        end
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    bot.log("info", string.format("%s: FILL %s %.5f @ $%.2f pnl=%.4f fee=%.4f",
        instance_name, fill.side, fill.size, fill.price, fill.closed_pnl, fill.fee))

    -- ── Entry fill: closed_pnl == 0 means opening (handles partial fills) ──
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
        -- Cancel ALL on exchange before refreshing SL/TP (catches trigger children)
        bot.cancel_all_exchange(config.coin)
        sl_oid = nil
        tp_oid = nil
        local pos = bot.get_position(config.coin)
        if pos and pos.size ~= 0 then
            place_sl_tp(entry_price, position_side, entry_regime or current_regime, math.abs(pos.size))
        end
        return
    end

    -- ── Exit fill: closed_pnl != 0 means closing ──
    if not in_position then
        bot.log("warn", string.format("%s: exit fill but not in_position (pnl=%.4f) — syncing",
            instance_name, fill.closed_pnl))
        return
    end

    last_trade = bot.time()
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
    bot.save_state("losing_streak", tostring(losing_streak))
    if losing_streak >= MAX_LOSING_STREAK then
        bot.save_state("streak_pause_until", tostring(streak_pause_until))
    end

    bot.log("info", string.format("%s: EXIT — pnl=%.4f (win rate: %d/%d = %.0f%%)",
        instance_name, fill.closed_pnl, win_count, trade_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0))

    -- Cancel ALL orders on exchange for this coin (catches orphaned trigger children)
    bot.cancel_all_exchange(config.coin)
    sl_oid = nil
    tp_oid = nil

    in_position = false
    position_side = nil
    entry_price = 0
    entry_regime = nil
    bot.save_state("has_position", "false")
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
        entry_regime = nil
        bot.cancel_all_exchange(config.coin)
        sl_oid = nil
        tp_oid = nil
    end
end
function on_shutdown()
    bot.log("info", string.format("%s: shutdown — %d trades, %d wins (%.0f%%), last regime=%s",
        instance_name, trade_count, win_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0,
        current_regime))
    bot.save_state("enabled", tostring(config.enabled))
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))
    bot.save_state("losing_streak", tostring(losing_streak))
    bot.save_state("has_position", tostring(in_position))

    if in_position then
        bot.log("info", instance_name .. ": position open, SL/TP on exchange")
    end
end
