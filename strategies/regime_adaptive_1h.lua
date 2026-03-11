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
    leverage      = 7,

    -- Regime detection (simplified: ADX-based, no dead zone)
    -- ADX < 25 = mean reversion, ADX >= 25 = trend
    adx_regime_threshold = 25,  -- single threshold, no transition gap

    -- Mean reversion params (low vol regime)
    mr_rsi_oversold   = 40,
    mr_rsi_overbought = 60,
    mr_sl_pct         = 1.2,
    mr_tp_pct         = 2.0,

    -- Trend params (high vol regime)
    tr_adx_min        = 20,     -- minimum ADX for trend entry (loosened)
    tr_sl_pct         = 2.0,
    tr_tp_pct         = 4.0,

    -- Position sizing
    entry_size    = 40.0,
    equity_pct    = 0.10,        -- compound: 10% of account per trade
    max_size      = 60.0,

    -- Timing
    check_sec     = 10,
    cooldown_sec  = 120,
    max_hold_sec  = 14400,      -- max hold 4h

    enabled       = true,
}

-- Instance name (e.g. "regime_adaptive_1h_eth")
local instance_name = "regime_adaptive_1h_" .. config.coin:lower()

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
local current_regime = "unknown" -- "mean_reversion", "trend", "unknown"
local entry_regime   = nil       -- regime when position was opened

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
    -- Simple ADX-based regime detection (no dead zone)
    if adx < config.adx_regime_threshold then
        return "mean_reversion"
    else
        return "trend"
    end
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
        bot.log("info", string.format("%s: ENTRY %s @ $%.2f ($%.0f) [%s mode]",
            instance_name, string.upper(side), price, trade_usd, current_regime))
        return oid
    end
    return nil
end

local function place_sl_tp(fill_price, side, regime, pos_size)
    local sl_pct = regime == "mean_reversion" and config.mr_sl_pct or config.tr_sl_pct
    local tp_pct = regime == "mean_reversion" and config.mr_tp_pct or config.tr_tp_pct
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

    bot.log("info", string.format("%s: SL=$%.2f (%.1f%%), TP=$%.2f (+%.1f%%) [%s]",
        instance_name, sl_price, sl_pct, tp_price, tp_pct, regime))
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
    entry_regime = nil
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "%s: Adaptive regime [MR: SL=%.1f%%/TP=%.1f%% | TR: SL=%.1f%%/TP=%.1f%%], check=%ds",
        instance_name, config.mr_sl_pct, config.mr_tp_pct,
        config.tr_sl_pct, config.tr_tp_pct, config.check_sec))

    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end

    local saved_trades = bot.load_state("trade_count")
    if saved_trades then trade_count = tonumber(saved_trades) or 0 end
    local saved_wins = bot.load_state("win_count")
    if saved_wins then win_count = tonumber(saved_wins) or 0 end

    local saved_regime = bot.load_state("entry_regime")
    if saved_regime then entry_regime = saved_regime end

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

    -- Get indicators (1h timeframe, 30 candles)
    local ind = bot.get_indicators(config.coin, "1h", 30, mid_price)
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

    if not bb_width or not adx or not rsi or not bb_upper or not bb_lower
       or not ema12 or not ema26 then
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
            sl_oid = nil
            tp_oid = nil
            entry_regime = nil
            return
        end

        -- Close on regime change (transition safety)
        if entry_regime and current_regime ~= "transition" and current_regime ~= entry_regime then
            close_position(string.format("regime changed %s → %s", entry_regime, current_regime))
            last_trade = now
            return
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
        -- LONG: ema_12 > ema_26 + adx > threshold
        if ema12 > ema26 and adx > config.tr_adx_min then
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

        -- SHORT: ema_12 < ema_26 + adx > threshold
        if ema12 < ema26 and adx > config.tr_adx_min then
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

    -- Entry fill
    if not in_position then
        entry_oid = nil
        entry_placed_at = 0
        in_position = true
        entry_price = fill.price
        position_side = fill.side == "buy" and "long" or "short"
        place_sl_tp(fill.price, position_side, entry_regime or current_regime, fill.size)
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
    entry_regime = nil
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
    bot.log("info", string.format("%s: shutdown — %d trades, %d wins (%.0f%%), last regime=%s",
        instance_name, trade_count, win_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0,
        current_regime))
    bot.save_state("enabled", tostring(config.enabled))
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))

    if in_position then
        bot.log("info", instance_name .. ": position open, SL/TP on exchange")
    end
end
