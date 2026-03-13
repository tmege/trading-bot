--[[
  Ichimoku Cloud Trend Strategy — Generic Multi-Coin

  Full Ichimoku system with RSI and ADX confirmation for trend trades.
  Uses trailing stop under Tenkan-sen for dynamic exit management.
  The COIN global is injected by the C engine before loading.
  Each instance runs independently with its own state.

  Logic:
  - LONG: ichi_bullish + price > cloud + RSI > 50 + ADX > 20
  - SHORT: price < cloud (below both senkou_a and senkou_b) + Tenkan < Kijun
  - Trailing stop: update SL to track Tenkan-sen level
  - Fixed backup: SL 3%, TP trailing under Tenkan
  - Size: $40, Lever: 3x
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = COIN or "ETH",
    -- leverage is set at engine/exchange level (config max_leverage)

    -- Removed RSI/ADX filters — Ichimoku is a self-sufficient trend system

    -- Position sizing
    entry_size    = 40.0,
    equity_pct    = 0.05,        -- 5% of account per trade (Kelly-aligned at ~40% WR)
    max_size      = 60.0,

    -- Exit targets (ATR-based SL, fixed TP backup)
    sl_atr_mult   = 2.0,        -- SL = 2.0x ATR (adapts to 4h volatility)
    sl_pct_min    = 2.0,        -- SL floor (never below 2%)
    sl_pct_max    = 4.5,        -- SL cap (never above 4.5%)
    tp_pct        = 8.0,        -- hard take profit % (wide for trend, trailing handles most exits)

    -- Trailing stop config
    trail_enabled  = true,       -- trail SL under Tenkan-sen
    trail_buffer   = 0.3,       -- 0.3% buffer below Tenkan for trail

    -- Cloud entry filter
    cloud_buffer_pct = 0.5,     -- min % distance from cloud edge to enter (avoids boundary oscillation)

    -- Timing
    check_sec     = 120,        -- check every 2min (4h swing — no need for faster)
    cooldown_sec  = 1800,       -- 30 min between trades (4h strategy)
    max_hold_sec  = 43200,      -- max hold 12h

    enabled       = true,
}

-- Instance name (e.g. "ichimoku_trend_4h_eth")
local instance_name = "ichimoku_trend_4h_" .. config.coin:lower()

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
local last_trail_px  = 0         -- last trailing stop price
local entry_atr      = 0         -- ATR at signal time, for dynamic SL

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
    -- Skip if position exists on this coin but we don't own it (another strategy)
    local existing = bot.get_position(config.coin)
    if existing and existing.size ~= 0 then
        if not in_position then
            bot.log("info", string.format("%s: SKIP — %s position on %s owned by another strategy",
                instance_name, existing.size > 0 and "long" or "short", config.coin))
            return nil
        end
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
        bot.log("info", string.format("%s: ENTRY %s @ $%.2f ($%.0f)",
            instance_name, string.upper(side), price, trade_usd))
        return oid
    end
    return nil
end

local function place_sl_tp(fill_price, side, pos_size)
    -- ATR-based SL, clamped to min/max
    local atr_val = entry_atr > 0 and entry_atr or (fill_price * 0.025)  -- fallback 2.5%
    local sl_pct = (config.sl_atr_mult * atr_val / fill_price) * 100
    sl_pct = math.max(config.sl_pct_min, math.min(config.sl_pct_max, sl_pct))

    local tp_pct = config.tp_pct
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

    last_trail_px = sl_price

    bot.log("info", string.format("%s: SL=$%.2f (%.1f%% ATR), TP=$%.2f (+%.1f%%)",
        instance_name, sl_price, sl_pct, tp_price, tp_pct))
end

local function update_trailing_stop(tenkan, side)
    if not config.trail_enabled then return end
    if not sl_oid then return end

    local pos = bot.get_position(config.coin)
    if not pos then return end
    local size = math.abs(pos.size)
    local new_sl

    if side == "long" then
        -- Trail SL up: set SL just below Tenkan-sen
        new_sl = tenkan * (1 - config.trail_buffer / 100)
        -- Only move SL up, never down
        if new_sl <= last_trail_px then return end
        -- Don't trail above current price (that would close immediately)
        local mid = bot.get_mid_price(config.coin)
        if mid and new_sl >= mid then return end
    else
        -- Trail SL down: set SL just above Tenkan-sen
        new_sl = tenkan * (1 + config.trail_buffer / 100)
        -- Only move SL down, never up
        if new_sl >= last_trail_px then return end
        -- Don't trail below current price
        local mid = bot.get_mid_price(config.coin)
        if mid and new_sl <= mid then return end
    end

    -- Cancel old SL, place new one
    bot.cancel(config.coin, sl_oid)

    if side == "long" then
        sl_oid = bot.place_trigger(config.coin, "sell", new_sl, size, new_sl, "sl")
    else
        sl_oid = bot.place_trigger(config.coin, "buy", new_sl, size, new_sl, "sl")
    end

    bot.log("info", string.format("%s: TRAIL SL moved $%.2f → $%.2f (Tenkan=$%.2f)",
        instance_name, last_trail_px, new_sl, tenkan))
    last_trail_px = new_sl
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
        last_trail_px = 0
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
    last_trail_px = 0
    bot.save_state("has_position", "false")
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "%s: Ichimoku Cloud — SL=%.1fxATR [%.1f-%.1f%%], TP=%.1f%%, trail=%s, check=%ds",
        instance_name, config.sl_atr_mult, config.sl_pct_min, config.sl_pct_max,
        config.tp_pct, config.trail_enabled and "ON" or "OFF", config.check_sec))

    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end

    local saved_trades = bot.load_state("trade_count")
    if saved_trades then trade_count = tonumber(saved_trades) or 0 end
    local saved_wins = bot.load_state("win_count")
    if saved_wins then win_count = tonumber(saved_wins) or 0 end
    local saved_streak = bot.load_state("losing_streak")
    if saved_streak then losing_streak = tonumber(saved_streak) or 0 end

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
            local ind = bot.get_indicators(config.coin, "4h", 52, pos.entry_px)
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

    -- Get indicators (4h timeframe, 52 candles for full Ichimoku + ATR)
    local ind = bot.get_indicators(config.coin, "4h", 52, mid_price)
    if not ind then
        bot.log("warn", instance_name .. ": get_indicators returned nil")
        return
    end

    local ichi_bullish = ind.ichi_bullish
    local senkou_a     = ind.ichi_senkou_a
    local senkou_b     = ind.ichi_senkou_b
    local tenkan       = ind.ichi_tenkan
    local kijun        = ind.ichi_kijun
    local atr          = ind.atr

    if not senkou_a or not senkou_b or not tenkan or not kijun then
        bot.log("warn", string.format("%s: missing Ichimoku fields", instance_name))
        return
    end

    -- Store ATR for dynamic SL on fill
    if atr and atr > 0 then entry_atr = atr end

    local cloud_top    = math.max(senkou_a, senkou_b)
    local cloud_bottom = math.min(senkou_a, senkou_b)
    local above_cloud  = mid_price > cloud_top
    local below_cloud  = mid_price < cloud_bottom

    bot.log("debug", string.format(
        "%s: %s=$%.2f cloud=[%.2f,%.2f] T=%.2f K=%.2f bull=%s",
        instance_name, config.coin, mid_price, cloud_bottom, cloud_top,
        tenkan, kijun, tostring(ichi_bullish)))

    -- ── In-position: trailing stop management ──

    if in_position then
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            in_position = false
            position_side = nil
            entry_price = 0
            bot.cancel_all_exchange(config.coin)
            sl_oid = nil
            tp_oid = nil
            last_trail_px = 0
            return
        end

        -- Update trailing stop based on Tenkan-sen
        update_trailing_stop(tenkan, position_side)

        -- Emergency close if price enters cloud against position
        if position_side == "long" and below_cloud then
            close_position("price dropped below cloud")
            last_trade = now
            return
        end
        if position_side == "short" and above_cloud then
            close_position("price rose above cloud")
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

    -- LONG: price above cloud WITH BUFFER + Tenkan > Kijun
    -- Buffer prevents entry right at cloud edge (causes rapid entry/exit oscillation)
    local cloud_buffer_up = cloud_top * config.cloud_buffer_pct / 100
    local above_with_buffer = mid_price > (cloud_top + cloud_buffer_up)
    local long_ichimoku = above_with_buffer and tenkan > kijun
    if ichi_bullish and above_with_buffer then long_ichimoku = true end

    if long_ichimoku then
        bot.log("info", string.format(
            "%s: ICHIMOKU LONG — above cloud+buffer ($%.2f > $%.2f+%.2f), T=%.2f > K=%.2f",
            instance_name, mid_price, cloud_top, cloud_buffer_up, tenkan, kijun))
        local oid = place_entry("long", mid_price)
        if oid then
            last_trade = now
            entry_time = now
        end
        return
    end

    -- SHORT: price below cloud WITH BUFFER + Tenkan < Kijun
    local cloud_buffer_dn = cloud_bottom * config.cloud_buffer_pct / 100
    if mid_price < (cloud_bottom - cloud_buffer_dn) and tenkan < kijun then
        bot.log("info", string.format(
            "%s: ICHIMOKU SHORT — below cloud-buffer ($%.2f < $%.2f-%.2f), T=%.2f < K=%.2f",
            instance_name, mid_price, cloud_bottom, cloud_buffer_dn, tenkan, kijun))
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
    last_trail_px = 0
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
        last_trail_px = 0
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
    bot.save_state("has_position", tostring(in_position))

    if in_position then
        bot.log("info", instance_name .. ": position open, SL/TP on exchange")
    end
end
