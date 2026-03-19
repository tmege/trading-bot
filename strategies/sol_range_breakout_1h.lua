--[[
  SOL Range Breakout + Bear Complement 1h — LIVE PRODUCTION
  Ne pas modifier sans re-backtest walk-forward

  Deux modes combines en une seule strategie (single-position):

  MODE 1 — Range Breakout (tous regimes):
    Detecte les phases de consolidation (range etroit sur 24 bougies),
    entre en breakout avec confirmation volume.
    TP 4.5% / SL 1.5%

  MODE 2 — Bear Complement B1 (bear regime seulement):
    Bear regime gate: close < SMA200 AND SMA50 < SMA200 AND ADX > 20
    Signal B1: RSI < 35 + MACD_hist < 0 + ATR < 1.2%
    SHORT-only. TP 6.0% / SL 2.0% (RR 3:1)

  Priorite: en bear regime, B1 est evalue EN PREMIER (avant range breakout).
  Si B1 match, on prend le short bear. Sinon, on fallback sur range breakout.

  Backtest valide:
    RB seul:   Sharpe 730j 1.66, DD 41.21%, 164 trades
    Combined:  Sharpe 730j 2.31, DD 33.09%, 313 trades, WR 40.9%
    Improvement: Sharpe +0.65, DD -8.1pp
    Bear B1:   Sharpe WF 1.09, 113 OOS, 5/6 folds, EV 1.924%
    C backtest: 54 trades, WR 44.4%, +113%, DD 19.2%, Sharpe 1.25 (bear seul 730j)
    Config: x5 leverage, 40% equity
    Fees: 0.06% round-trip inclus

  Version : 2.0.0
  Date validation : 2026-03
]]

-- ── Configuration ────────────────────────────────────────────────────────
local config = {
    coin            = COIN or "SOL",

    -- Sizing (calibre pour DD < 35%)
    equity_pct      = 0.40,
    leverage        = 5,
    max_size        = 9999.0,
    entry_size      = 50.0,

    -- Timing
    check_sec       = 60,
    cooldown_sec    = 14400,        -- 4h entre trades
    max_hold_sec    = 172800,       -- 48h max hold

    -- Range breakout params
    lookback_bars   = 24,
    range_threshold = 0.04,         -- max 4%
    range_min       = 0.005,        -- min 0.5% (evite bruit)
    vol_multiplier  = 1.2,

    -- TP/SL — Range breakout (overridable via GRID_TP/GRID_SL)
    rb_tp_pct       = GRID_TP or 4.5,
    rb_sl_pct       = GRID_SL or 1.5,

    -- Bear complement B1 params (overridable via GRID_TP/GRID_SL)
    bear_tp_pct     = GRID_TP or 6.0,
    bear_sl_pct     = GRID_SL or 2.0,
    bear_atr_threshold = 0.012,     -- ATR < 1.2% du prix
    bear_rsi_short  = 35,

    -- ATR-adaptive sizing
    atr_baseline    = 0.008,        -- ATR% median historique SOL
    atr_size_min    = 0.5,          -- min multiplier (haute vol)
    atr_size_max    = 1.5,          -- max multiplier (basse vol)

    -- Trailing stop (disabled: cuts winners short on RR 3:1 setup)
    -- To re-enable: set trail_activate to ~3.5% (near TP) with offset ~2.0%
    trail_activate  = 999,          -- effectively disabled
    trail_offset    = 1.0,          -- SL trails at best_px - 1.0%
    trail_step      = 0.5,          -- ratchet by 0.5% increments

    -- Guard
    min_atr_pct     = 0.001,

    enabled         = true,
    version         = "2.0.0",
    validated_date  = "2026-03",
}

-- ── Monitoring thresholds ────────────────────────────────────────────────
local MONITOR = {
    pause_consec_losses   = 3,
    pause_daily_dd_pct    = 0.08,
    pause_weekly_dd_pct   = 0.18,
    stop_monthly_dd_pct   = 0.25,
    stop_total_dd_pct     = 0.34,
}

local instance_name = "sol_rb_bear_1h_" .. config.coin:lower()

-- ATR-adaptive sizing state (set in on_tick before signal scan)
local current_atr_pct = 0

-- ── Position sizing avec drawdown guard ──────────────────────────────────
local peak_equity   = 0
local consec_losses = 0
local paused        = false
local pause_until   = 0
local pause_reason  = ""

local function get_trade_size()
    local acct = bot.get_account_value()
    if not acct or acct <= 0 then return config.entry_size end

    if acct > peak_equity then peak_equity = acct end

    local dd_mult = 1.0

    if peak_equity > 0 then
        local dd_pct = (peak_equity - acct) / peak_equity
        if dd_pct > MONITOR.stop_total_dd_pct then
            dd_mult = 0
            paused = true
            pause_reason = string.format("total_dd=%.1f%%", dd_pct * 100)
        elseif dd_pct > MONITOR.stop_monthly_dd_pct then
            dd_mult = 0
            paused = true
            pause_reason = string.format("dd=%.1f%%", dd_pct * 100)
        elseif dd_pct > MONITOR.pause_weekly_dd_pct then
            dd_mult = 0.25
        elseif dd_pct > MONITOR.pause_daily_dd_pct then
            dd_mult = 0.50
        end
    end

    if dd_mult == 0 then return 0 end

    -- ATR-adaptive multiplier: smaller in high vol, bigger in low vol
    local atr_mult = 1.0
    if current_atr_pct > 0 and config.atr_baseline > 0 then
        atr_mult = config.atr_baseline / current_atr_pct
        atr_mult = math.max(config.atr_size_min, math.min(config.atr_size_max, atr_mult))
    end

    local size = acct * config.equity_pct * config.leverage * dd_mult * atr_mult
    return math.min(size, config.max_size)
end

-- ── State ────────────────────────────────────────────────────────────────
local last_check      = 0
local last_trade      = 0
local in_position     = false
local position_side   = nil
local entry_price     = 0
local entry_time      = 0
local sl_oid          = nil
local tp_oid          = nil
local entry_oid       = nil
local entry_placed_at = 0
local ENTRY_TIMEOUT   = 90
local trade_count     = 0
local win_count       = 0
local active_signal   = ""  -- "RB" or "B1"

-- Trailing stop state
local trail_active    = false
local trail_best_px   = 0
local trail_last_sl   = 0

-- ── Helpers ──────────────────────────────────────────────────────────────
local function place_entry(side, mid)
    if in_position then return nil end

    if entry_oid then
        bot.cancel(config.coin, entry_oid)
        entry_oid = nil
    end

    local trade_usd = get_trade_size()
    if trade_usd <= 0 then
        bot.log("warn", string.format("%s: SKIPPED — %s",
            instance_name, paused and pause_reason or "drawdown guard"))
        return nil
    end

    local price = side == "long"
        and mid * 0.9998
        or  mid * 1.0002
    local size = trade_usd / mid
    local order_side = side == "long" and "buy" or "sell"
    local oid = bot.place_limit(config.coin, order_side, price, size, { tif = "alo" })
    if oid then
        entry_oid = oid
        entry_placed_at = bot.time()
        bot.log("info", string.format("%s: ENTRY %s @ $%.2f ($%.0f, x%d) [%s]",
            instance_name, string.upper(side), price, trade_usd, config.leverage, active_signal))
        return oid
    end
    return nil
end

local function place_sl_tp(fill_price, side, pos_size)
    -- TP/SL depend on which signal is active
    local tp_pct = active_signal == "B1" and config.bear_tp_pct or config.rb_tp_pct
    local sl_pct = active_signal == "B1" and config.bear_sl_pct or config.rb_sl_pct

    local sl_price, tp_price
    if side == "long" then
        sl_price = fill_price * (1 - sl_pct / 100)
        tp_price = fill_price * (1 + tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "sell", sl_price, pos_size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "sell", tp_price, pos_size, tp_price, "tp")
    else
        sl_price = fill_price * (1 + sl_pct / 100)
        tp_price = fill_price * (1 - tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "buy", sl_price, pos_size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "buy", tp_price, pos_size, tp_price, "tp")
    end

    bot.log("info", string.format("%s: SL=$%.2f (-%.1f%%), TP=$%.2f (+%.1f%%) [%s]",
        instance_name, sl_price, sl_pct, tp_price, tp_pct, active_signal))
end

local function close_position(reason)
    if not in_position then return end
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
    local oid = bot.place_limit(config.coin, side, mid, size, { tif = "ioc", reduce_only = true })
    if not oid then
        local px = side == "sell" and mid * 0.99 or mid * 1.01
        bot.place_limit(config.coin, side, px, size, { tif = "ioc", reduce_only = true })
    end
    bot.log("info", string.format("%s: CLOSE — %s [%s]", instance_name, reason, active_signal))

    in_position = false
    position_side = nil
    entry_price = 0
    bot.save_state("has_position", "false")
end

-- ── Bear regime detection ────────────────────────────────────────────────
local function is_bear_regime(ind, mid)
    if not ind.sma_200 or not ind.sma_50 or not ind.adx then
        return false
    end
    return mid < ind.sma_200
       and ind.sma_50 < ind.sma_200
       and ind.adx > 20
end

-- ── Bear B1 signal: RSI < 35 + MACD_hist < 0 + ATR < 1.2% ──────────────
local function check_bear_b1(ind, mid)
    if not ind.rsi or not ind.macd_histogram or not ind.atr then
        return false
    end
    return ind.rsi < config.bear_rsi_short
       and ind.macd_histogram < 0
       and (ind.atr / mid) < config.bear_atr_threshold
end

-- ── Range breakout signal detection ──────────────────────────────────────
local function check_range_breakout(candles, n, mid_price)
    if n < config.lookback_bars + 1 then return nil end

    local range_hi = -math.huge
    local range_lo = math.huge
    local vol_sum  = 0

    local start_idx = n - config.lookback_bars
    for i = start_idx, n - 1 do
        local hi = candles[i].h or candles[i].high
        local lo = candles[i].l or candles[i].low
        local vol = candles[i].v or candles[i].volume
        if not hi or not lo or not vol then return nil end
        if hi > range_hi then range_hi = hi end
        if lo < range_lo then range_lo = lo end
        vol_sum = vol_sum + vol
    end

    if range_lo <= 0 then return nil end

    local range_pct = (range_hi - range_lo) / range_lo
    if range_pct > config.range_threshold then return nil end
    if range_pct < config.range_min then return nil end

    local avg_vol = vol_sum / config.lookback_bars
    local cur_vol = candles[n].v or candles[n].volume
    if not cur_vol or avg_vol <= 0 or cur_vol <= avg_vol * config.vol_multiplier then return nil end

    local cur_close = mid_price
    if cur_close > range_hi then
        return "long"
    elseif cur_close < range_lo then
        return "short"
    end

    return nil
end

-- ── Callbacks ────────────────────────────────────────────────────────────
function on_init()
    bot.log("info", string.format(
        "%s: SOL RB+Bear v%s [EQ=%.0f%%, x%d, RB TP=%.1f%%/SL=%.1f%%, B1 TP=%.1f%%/SL=%.1f%%]",
        instance_name, config.version,
        config.equity_pct * 100, config.leverage,
        config.rb_tp_pct, config.rb_sl_pct,
        config.bear_tp_pct, config.bear_sl_pct))

    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end
    local st = bot.load_state("trade_count")
    if st then trade_count = tonumber(st) or 0 end
    local sw = bot.load_state("win_count")
    if sw then win_count = tonumber(sw) or 0 end
    local sc = bot.load_state("consec_losses")
    if sc then consec_losses = tonumber(sc) or 0 end
    local sp = bot.load_state("peak_equity")
    if sp then peak_equity = tonumber(sp) or 0 end
    active_signal = bot.load_state("active_signal") or ""

    local had = bot.load_state("has_position")
    if had == "true" then
        local pos = bot.get_position(config.coin)
        if pos and pos.size ~= 0 then
            in_position = true
            entry_price = pos.entry_px
            position_side = pos.size > 0 and "long" or "short"
            entry_time = bot.time()
            bot.cancel_all_exchange(config.coin)
            place_sl_tp(entry_price, position_side, math.abs(pos.size))
            bot.log("info", string.format("%s: restored %s @ $%.2f [%s]",
                instance_name, position_side, entry_price, active_signal))
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

    -- Max hold: force close
    if in_position and now - entry_time > config.max_hold_sec then
        close_position(string.format("max hold %ds", config.max_hold_sec))
        last_trade = now
        return
    end

    -- In position: monitor + trailing stop
    if in_position then
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            in_position = false
            position_side = nil
            entry_price = 0
            trail_active = false
            trail_best_px = 0
            trail_last_sl = 0
            bot.cancel_all_exchange(config.coin)
            sl_oid = nil
            tp_oid = nil
            return
        end

        -- Trailing stop logic
        if entry_price > 0 and mid_price > 0 then
            local profit_pct
            if position_side == "long" then
                profit_pct = (mid_price - entry_price) / entry_price * 100
            else
                profit_pct = (entry_price - mid_price) / entry_price * 100
            end

            if profit_pct >= config.trail_activate then
                if not trail_active then
                    trail_active = true
                    trail_best_px = mid_price
                    trail_last_sl = 0
                    bot.log("info", string.format("%s: TRAIL ACTIVATED at +%.1f%% mid=$%.2f [%s]",
                        instance_name, profit_pct, mid_price, active_signal))
                end

                -- Update best price
                if position_side == "long" and mid_price > trail_best_px then
                    trail_best_px = mid_price
                elseif position_side == "short" and mid_price < trail_best_px then
                    trail_best_px = mid_price
                end

                -- Calculate new trailing SL
                local new_sl
                if position_side == "long" then
                    new_sl = trail_best_px * (1 - config.trail_offset / 100)
                else
                    new_sl = trail_best_px * (1 + config.trail_offset / 100)
                end

                -- Ratchet: only move SL if it improved by trail_step
                local should_move = false
                if trail_last_sl == 0 then
                    should_move = true
                elseif position_side == "long" then
                    should_move = (new_sl - trail_last_sl) / entry_price * 100 >= config.trail_step
                else
                    should_move = (trail_last_sl - new_sl) / entry_price * 100 >= config.trail_step
                end

                if should_move then
                    if sl_oid then
                        bot.cancel(config.coin, sl_oid)
                        sl_oid = nil
                    end
                    local pos_size = math.abs(pos.size)
                    if position_side == "long" then
                        sl_oid = bot.place_trigger(config.coin, "sell", new_sl, pos_size, new_sl, "sl")
                    else
                        sl_oid = bot.place_trigger(config.coin, "buy", new_sl, pos_size, new_sl, "sl")
                    end
                    trail_last_sl = new_sl
                    bot.log("info", string.format("%s: TRAIL SL moved to $%.2f (best=$%.2f, +%.1f%%) [%s]",
                        instance_name, new_sl, trail_best_px, profit_pct, active_signal))
                end
            end
        end
        return
    end

    -- Pending entry guard
    if entry_oid then
        if now - entry_placed_at > ENTRY_TIMEOUT then
            bot.cancel(config.coin, entry_oid)
            entry_oid = nil
        end
        return
    end

    -- Cooldown
    if now - last_trade < config.cooldown_sec then return end

    -- Guard: sync in_position with actual exchange position
    local guard_pos = bot.get_position(config.coin)
    if guard_pos and guard_pos.size ~= 0 then
        in_position = true
        position_side = guard_pos.size > 0 and "long" or "short"
        entry_price = guard_pos.entry_px
        close_position("orphaned position")
        last_trade = now
        return
    end

    -- Paused check
    if paused then
        if now >= pause_until then
            paused = false
            consec_losses = 0
            pause_reason = ""
            bot.log("info", string.format("%s: PAUSE EXPIRED — resuming trading", instance_name))
        else
            return
        end
    end

    -- Get indicators (full history)
    local ind = bot.get_indicators(config.coin, "1h", 0, mid_price)
    if not ind or not ind.atr then return end

    local atr_pct = ind.atr / mid_price
    current_atr_pct = atr_pct
    if atr_pct < config.min_atr_pct then return end

    -- ── SIGNAL PRIORITY: Bear B1 first (if in bear regime), then Range Breakout

    local bear = is_bear_regime(ind, mid_price)

    if bear and check_bear_b1(ind, mid_price) then
        -- Bear B1 signal: SHORT only
        active_signal = "B1"
        bot.save_state("active_signal", active_signal)

        bot.log("info", string.format(
            "%s: SIGNAL B1_bear_mom (SHORT) — RSI=%.0f ADX=%.0f ATR%%=%.3f MACD=%.4f SMA50=%.2f SMA200=%.2f mid=$%.2f",
            instance_name, ind.rsi, ind.adx, atr_pct * 100, ind.macd_histogram,
            ind.sma_50, ind.sma_200, mid_price))

        entry_time = now
        local oid = place_entry("short", mid_price)
        if oid then last_trade = now end
        return
    end

    -- Range breakout (all regimes)
    local candles = bot.get_candles(config.coin, "1h", config.lookback_bars + 5)
    if not candles or #candles < config.lookback_bars + 1 then return end

    local rb_side = check_range_breakout(candles, #candles, mid_price)
    if not rb_side then return end

    active_signal = "RB"
    bot.save_state("active_signal", active_signal)

    local range_hi = -math.huge
    local range_lo = math.huge
    local n = #candles
    for i = n - config.lookback_bars, n - 1 do
        local hi = candles[i].h or candles[i].high
        local lo = candles[i].l or candles[i].low
        if hi and hi > range_hi then range_hi = hi end
        if lo and lo < range_lo then range_lo = lo end
    end
    local range_pct = (range_hi - range_lo) / range_lo * 100

    bot.log("info", string.format(
        "%s: SIGNAL RB_%s — range=%.2f%% hi=$%.2f lo=$%.2f ATR%%=%.3f%s",
        instance_name, string.upper(rb_side),
        range_pct, range_hi, range_lo, atr_pct * 100,
        bear and " [BEAR]" or ""))

    entry_time = now
    local oid = place_entry(rb_side, mid_price)
    if oid then last_trade = now end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    bot.log("info", string.format("%s: FILL %s %.5f @ $%.2f pnl=%.4f [%s]",
        instance_name, fill.side, fill.size, fill.price, fill.closed_pnl, active_signal))

    -- Entry fill (closed_pnl == 0)
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
        bot.cancel_all_exchange(config.coin)
        sl_oid = nil
        tp_oid = nil
        local pos = bot.get_position(config.coin)
        if pos and pos.size ~= 0 then
            place_sl_tp(entry_price, position_side, math.abs(pos.size))
        end
        return
    end

    -- Exit fill
    last_trade = bot.time()
    trade_count = trade_count + 1
    if fill.closed_pnl > 0 then
        win_count = win_count + 1
        consec_losses = 0
        paused = false
        pause_reason = ""
    else
        consec_losses = consec_losses + 1
        if consec_losses >= MONITOR.pause_consec_losses then
            paused = true
            pause_until = bot.time() + config.cooldown_sec * 2
            pause_reason = string.format("consecutive_losses=%d", consec_losses)
            bot.log("warn", string.format("%s: PAUSE 8h — %d consecutive losses, resume at +%ds",
                instance_name, consec_losses, config.cooldown_sec * 2))
        end
    end
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))
    bot.save_state("consec_losses", tostring(consec_losses))

    bot.log("info", string.format(
        "%s: EXIT pnl=%.4f [%s] (W/L: %d/%d = %.0f%%, consec_loss=%d)",
        instance_name, fill.closed_pnl, active_signal,
        win_count, trade_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0,
        consec_losses))

    bot.cancel_all_exchange(config.coin)
    sl_oid = nil
    tp_oid = nil
    in_position = false
    position_side = nil
    entry_price = 0
    trail_active = false
    trail_best_px = 0
    trail_last_sl = 0
    bot.save_state("has_position", "false")
end

function on_timer()
    if entry_oid and not in_position then
        if bot.time() - entry_placed_at > ENTRY_TIMEOUT then
            bot.cancel(config.coin, entry_oid)
            entry_oid = nil
        end
    end

    if not in_position then return end
    local pos = bot.get_position(config.coin)
    if not pos or pos.size == 0 then
        in_position = false
        position_side = nil
        entry_price = 0
        trail_active = false
        trail_best_px = 0
        trail_last_sl = 0
        bot.cancel_all_exchange(config.coin)
        sl_oid = nil
        tp_oid = nil
    end

    if paused and bot.time() >= pause_until then
        paused = false
        consec_losses = 0
        pause_reason = ""
        bot.log("info", string.format("%s: PAUSE EXPIRED (timer) — resuming", instance_name))
    end
end

function on_shutdown()
    bot.log("info", string.format("%s: shutdown — %d trades, %d wins (%.0f%%)",
        instance_name, trade_count, win_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0))
    bot.save_state("enabled", tostring(config.enabled))
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))
    bot.save_state("consec_losses", tostring(consec_losses))
    bot.save_state("has_position", tostring(in_position))
    bot.save_state("active_signal", active_signal or "")

    local acct = bot.get_account_value()
    if acct and acct > peak_equity then peak_equity = acct end
    bot.save_state("peak_equity", tostring(peak_equity))
end
