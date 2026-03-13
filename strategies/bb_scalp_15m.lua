--[[
  Scalping / Mean Reversion Strategy — Generic Multi-Coin

  BB mean reversion strategy — profits in ranging/sideways markets.
  The COIN global is injected by the C engine before loading.
  Each instance runs independently with its own state.

  Logic:
  - BUY when price touches lower Bollinger Band + RSI < 35 (oversold bounce)
  - SELL when price reaches middle BB or upper BB
  - SELL SHORT when price touches upper BB + RSI > 65 (overbought rejection)
  - Cover short at middle BB or lower BB
  - Tight stops: 1.5% max loss per trade
  - Quick trades: target 1-3% profit per scalp
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = COIN or "ETH",
    leverage      = 10,

    -- Bollinger Bands
    bb_period     = 20,
    bb_std        = 2.0,

    -- Entry conditions (widened for more signals)
    rsi_oversold  = 35,          -- buy when RSI < this at lower BB (was 40, tightened)
    rsi_overbought = 65,         -- short when RSI > this at upper BB (was 60, tightened)
    bb_touch_pct  = 0.5,         -- price within 0.5% of BB = "touching"

    -- Position sizing (split across coins: $40 each)
    entry_size    = 40.0,        -- USD per scalp (fallback/minimum)
    equity_pct    = 0.08,        -- 8% of account per trade (scalp: more trades, smaller size)
    max_size      = 60.0,        -- hard cap

    -- Exit targets
    tp_mid_bb     = true,        -- take profit at BB target (graduated: 60% to opposite band, min mid-BB)
    tp_pct        = 2.0,         -- hard cap TP trigger on exchange (was 3.0, unreachable for MR)
    sl_atr_mult   = 1.2,         -- SL = 1.2x ATR (ATR-based, adapts to volatility)
    sl_pct_min    = 0.8,         -- SL floor (never below 0.8%)
    sl_pct_max    = 2.5,         -- SL cap (never above 2.5%)

    -- Timing
    check_sec     = 5,           -- check every 5s (fast scalping)
    cooldown_sec  = 60,          -- 1 min between scalps
    max_hold_sec  = 2700,        -- max hold 45min (3 candle periods — scalp failed after this)

    -- Filter
    min_bb_width  = 1.0,         -- minimum BB width % (skip if too tight = no vol)
    max_bb_width  = 8.0,         -- skip if BB too wide (trending, not ranging)
    use_htf_filter = true,       -- 1h EMA trend filter: don't scalp against the trend

    enabled       = true,
}

-- Instance name for logging and advisory (e.g. "bb_scalp_15m_eth", "bb_scalp_15m_btc")
local instance_name = "bb_scalp_15m_" .. config.coin:lower()

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
local entry_atr      = 0         -- ATR at signal time, for dynamic SL

-- ── Crash protection ──────────────────────────────────────────────────────
local price_history     = {}      -- {price, time} ring buffer
local PRICE_HIST_SIZE   = 30      -- track last 30 price samples
local VELOCITY_THRESH   = 10.0    -- skip entry if >10% move in window
local losing_streak     = 0
local MAX_LOSING_STREAK = 3       -- pause 5 min after 3 consecutive losses
local streak_pause_until = 0

local function velocity_check(mid_price, now)
    -- Add to history
    table.insert(price_history, {p = mid_price, t = now})
    if #price_history > PRICE_HIST_SIZE then
        table.remove(price_history, 1)
    end
    -- Need at least 5 samples
    if #price_history < 5 then return true end
    -- Find min/max in window
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

local function near_band(price, band, pct)
    return math.abs(price - band) / band * 100 < pct
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
        and mid * 0.9998   -- buy slightly below mid (stay on book)
        or  mid * 1.0002   -- sell slightly above mid (stay on book)
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
    -- ATR-based SL, clamped to min/max
    local atr_val = entry_atr > 0 and entry_atr or (fill_price * 0.015)  -- fallback 1.5%
    local sl_pct = (config.sl_atr_mult * atr_val / fill_price) * 100
    sl_pct = math.max(config.sl_pct_min, math.min(config.sl_pct_max, sl_pct))

    -- Hard TP = max of config.tp_pct or 1.5x SL (safety net, mid-BB exit handles most closes)
    local tp_pct = math.max(config.tp_pct, sl_pct * 1.5)

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

    bot.log("info", string.format("%s: SL=$%.2f (%.1f%% ATR), TP=$%.2f (+%.1f%%)",
        instance_name, sl_price, sl_pct, tp_price, tp_pct))
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
    bot.save_state("has_position", "false")
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "%s: BB(%d,%.1f), SL=%.1fxATR [%.1f-%.1f%%], TP=%.1f%%, interval=%ds",
        instance_name, config.bb_period, config.bb_std, config.sl_atr_mult,
        config.sl_pct_min, config.sl_pct_max, config.tp_pct, config.check_sec))

    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end

    local saved_trades = bot.load_state("trade_count")
    if saved_trades then trade_count = tonumber(saved_trades) or 0 end
    local saved_wins = bot.load_state("win_count")
    if saved_wins then win_count = tonumber(saved_wins) or 0 end

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
            local ind = bot.get_indicators(config.coin, "15m", 50, pos.entry_px)
            if ind and ind.atr and ind.atr > 0 then entry_atr = ind.atr end
            place_sl_tp(entry_price, position_side, math.abs(pos.size))
            bot.log("info", string.format("%s: re-placed SL/TP after restart", instance_name))
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

    -- Check if position still exists
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

        -- Graduated BB target: 60% of entry→opposite band, minimum mid-BB
        if in_position and config.tp_mid_bb then
            local ind = bot.get_indicators(config.coin, "15m", 50, mid_price)
            if ind and ind.bb_middle and ind.bb_upper and ind.bb_lower then
                local target
                if position_side == "long" then
                    target = math.max(entry_price + (ind.bb_upper - entry_price) * 0.6, ind.bb_middle)
                    if mid_price >= target then
                        close_position(string.format("BB target ($%.2f >= $%.2f)", mid_price, target))
                        last_trade = now
                        return
                    end
                elseif position_side == "short" then
                    target = math.min(entry_price - (entry_price - ind.bb_lower) * 0.6, ind.bb_middle)
                    if mid_price <= target then
                        close_position(string.format("BB target ($%.2f <= $%.2f)", mid_price, target))
                        last_trade = now
                        return
                    end
                end
            end
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
        losing_streak = 0  -- reset after pause expires
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
    local atr = ind.atr

    if not rsi or not bb_upper or not bb_lower or not bb_mid then
        bot.log("warn", string.format("%s: missing fields — rsi=%s bb=%s/%s/%s",
            instance_name, tostring(rsi), tostring(bb_upper), tostring(bb_lower),
            tostring(bb_mid)))
        return
    end

    -- Store ATR for dynamic SL on fill (fallback if nil)
    entry_atr = atr or 0

    -- Check BB width filter
    local width = bb_width_pct(bb_upper, bb_lower, bb_mid)
    bot.log("debug", string.format("%s: %s=$%.2f BB=[%.2f / %.2f / %.2f] w=%.1f%% RSI=%.1f",
        instance_name, config.coin, mid_price, bb_lower, bb_mid, bb_upper, width, rsi))
    if width < config.min_bb_width then
        bot.log("debug", string.format("%s: BB too tight (%.1f%%), skipping", instance_name, width))
        return
    end
    if width > config.max_bb_width then
        bot.log("debug", string.format("%s: BB too wide (%.1f%%), trending", instance_name, width))
        return
    end

    -- Higher-TF trend filter: only block scalps against STRONG 1h trends (ADX > 30)
    -- Mild trends are fine for mean reversion — only block when momentum is overwhelming
    local htf_trend = "neutral"
    if config.use_htf_filter then
        local htf = bot.get_indicators(config.coin, "1h", 50, mid_price)
        if htf and htf.ema_12 and htf.ema_26 and htf.adx then
            if htf.ema_12 > htf.ema_26 and htf.adx > 30 then
                htf_trend = "strong_bullish"
            elseif htf.ema_12 < htf.ema_26 and htf.adx > 30 then
                htf_trend = "strong_bearish"
            end
        end
    end

    -- LONG: price at or below lower BB + RSI oversold
    -- Note: no MACD filter — mean reversion enters AGAINST momentum by design
    -- HTF filter: only block if strong 1h downtrend (htf_adx > 30 + bearish)
    if mid_price <= bb_lower * (1 + config.bb_touch_pct / 100) and rsi < config.rsi_oversold
       and htf_trend ~= "strong_bearish" then
        bot.log("info", string.format(
            "%s: LOWER BB touch ($%.2f <= $%.2f) + RSI=%.0f htf=%s → LONG",
            instance_name, mid_price, bb_lower, rsi, htf_trend))
        local oid = place_entry("long", mid_price)
        if oid then
            last_trade = now
            entry_time = now
        end
        return
    end

    -- SHORT: price at or above upper BB + RSI overbought
    if mid_price >= bb_upper * (1 - config.bb_touch_pct / 100) and rsi > config.rsi_overbought
       and htf_trend ~= "strong_bullish" then
        bot.log("info", string.format(
            "%s: UPPER BB touch ($%.2f >= $%.2f) + RSI=%.0f htf=%s → SHORT",
            instance_name, mid_price, bb_upper, rsi, htf_trend))
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
            place_sl_tp(entry_price, position_side, math.abs(pos.size))
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
        losing_streak = 0
        win_count = win_count + 1
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
    bot.save_state("losing_streak", tostring(losing_streak))
    bot.save_state("has_position", tostring(in_position))

    if in_position then
        bot.log("info", instance_name .. ": position open, SL/TP on exchange")
    end
end
