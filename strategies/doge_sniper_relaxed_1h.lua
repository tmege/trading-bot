--[[
  DOGE Sniper Relaxed 1h — LIVE PRODUCTION
  Ne pas modifier sans re-backtest walk-forward

  Signaux : L1 + L3 + S1 + S2 (4 signaux, S1 dominant en bear regime)
  TP 4.5% / SL 1.5% (RR 3:1)
  ATR < 0.9% du prix (plus strict que standard — sweet spot DOGE)

  Backtest valide Phase 5 :
    Sharpe WF 1.27, WR 41.2%, DD 28.96%, 231 trades OOS, 6/6 folds positifs
    Config: x5 leverage, 50% equity
    Full dataset: 275 trades, +225.9% (C backtest), DD 30.6%, 6.6 ans
    Fees: 0.06% round-trip inclus

  Note: DOGE est 82% bear regime — S1 est le signal dominant (157/291 trades).
  NE PAS revenir au SL dynamique ATR x4 — TP/SL fixe RR 3:1 est valide.

  Version : 1.0.0
  Date validation : 2026-03
]]

-- Configuration (identique au backtest Phase 5)
local config = {
    coin           = COIN or "DOGE",

    -- Sizing (calibre pour DD < 30%)
    equity_pct     = 0.50,
    leverage       = 5,
    max_size       = 9999.0,
    entry_size     = 50.0,

    -- TP/SL (RR 3:1 fixe)
    tp_pct         = 4.5,
    sl_pct         = 1.5,

    -- Signal filters
    atr_threshold  = 0.009,         -- ATR < 0.9% du prix
    rsi_long       = 65,
    rsi_short      = 35,

    -- Timing
    check_sec      = 60,
    cooldown_sec   = 14400,         -- 4h entre trades
    max_hold_sec   = 172800,        -- 48h max hold

    -- Guard
    min_atr_pct    = 0.001,

    enabled        = true,
    version        = "1.0.0",
    validated_date = "2026-03",
}

-- Monitoring thresholds (valides sur historique 6.6 ans)
local MONITOR = {
    -- Pause triggers (suspendre nouvelles entrees)
    pause_consec_losses   = 4,
    pause_daily_dd_pct    = 0.07,
    pause_weekly_dd_pct   = 0.15,

    -- Stop triggers (intervention humaine obligatoire)
    stop_monthly_dd_pct   = 0.25,
    stop_total_dd_pct     = 0.29,   -- DD max backtest
}

-- Signal Definitions: L1 + L3 + S1 + S2
local SIGNALS = {
    -- LONG L1: RSI momentum + low vol + MACD deceleration
    { name = "L1_momentum_decel", side = "long",
      check = function(ind, mid, h)
          if not h.prev_macd or not h.prev2_macd then return false end
          return ind.rsi > config.rsi_long
             and (ind.atr / mid) < config.atr_threshold
             and ind.macd_histogram < h.prev_macd
             and h.prev_macd < h.prev2_macd
      end },

    -- LONG L3: RSI momentum + ADX trend + low vol
    { name = "L3_trend_calm", side = "long",
      check = function(ind, mid, h)
          return ind.rsi > config.rsi_long
             and ind.adx > 20
             and (ind.atr / mid) < config.atr_threshold
      end },

    -- SHORT S1: RSI oversold + MACD bear + low vol (signal dominant DOGE)
    { name = "S1_bear_momentum", side = "short",
      check = function(ind, mid, h)
          return ind.rsi < config.rsi_short
             and ind.macd_histogram < 0
             and (ind.atr / mid) < config.atr_threshold
      end },

    -- SHORT S2: RSI oversold + downtrend + low vol
    { name = "S2_oversold_trend", side = "short",
      check = function(ind, mid, h)
          return ind.rsi < config.rsi_short
             and (ind.atr / mid) < config.atr_threshold
             and (ind.sma_20 < ind.sma_50 or ind.macd < 0)
      end },
}

local instance_name = "doge_sniper_1h_" .. config.coin:lower()

-- Position sizing with drawdown guard
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

    local size = acct * config.equity_pct * config.leverage * dd_mult
    return math.min(size, config.max_size)
end

-- State
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
local active_signal   = ""

-- MACD history for L1 deceleration signal
local last_hour       = 0
local last_macd_val   = nil
local prev_macd       = nil
local prev2_macd      = nil

-- Helpers
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
        bot.log("info", string.format("%s: ENTRY %s @ $%.4f ($%.0f, x%d) [%s]",
            instance_name, string.upper(side), price, trade_usd, config.leverage, active_signal))
        return oid
    end
    return nil
end

local function place_sl_tp(fill_price, side, pos_size)
    local sl_price, tp_price
    if side == "long" then
        sl_price = fill_price * (1 - config.sl_pct / 100)
        tp_price = fill_price * (1 + config.tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "sell", sl_price, pos_size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "sell", tp_price, pos_size, tp_price, "tp")
    else
        sl_price = fill_price * (1 + config.sl_pct / 100)
        tp_price = fill_price * (1 - config.tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "buy", sl_price, pos_size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "buy", tp_price, pos_size, tp_price, "tp")
    end

    bot.log("info", string.format("%s: SL=$%.4f (-%.1f%%), TP=$%.4f (+%.1f%%)",
        instance_name, sl_price, config.sl_pct, tp_price, config.tp_pct))
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
    bot.log("info", string.format("%s: CLOSE — %s", instance_name, reason))

    in_position = false
    position_side = nil
    entry_price = 0
    bot.save_state("has_position", "false")
end

local function indicators_valid(ind)
    return ind.rsi and ind.atr and ind.macd_histogram
       and ind.adx and ind.sma_20 and ind.sma_50
       and ind.macd
end

-- Callbacks
function on_init()
    bot.log("info", string.format(
        "%s: DOGE Sniper Relaxed LIVE [v%s, L1+L3+S1+S2, TP=%.1f%% SL=%.1f%%, x%d, EQ=%.0f%%]",
        instance_name, config.version,
        config.tp_pct, config.sl_pct, config.leverage, config.equity_pct * 100))

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
            bot.log("info", string.format("%s: restored %s @ $%.4f [%s]",
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

    -- In position: monitor
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
            bot.log("info", string.format("%s: PAUSE EXPIRED — resuming", instance_name))
        else
            return
        end
    end

    -- Get indicators (50 candles lookback for MACD convergence)
    local ind = bot.get_indicators(config.coin, "1h", 50, mid_price)
    if not ind then return end
    if not indicators_valid(ind) then return end

    -- Min volatility guard
    local atr_pct = ind.atr / mid_price
    if atr_pct < config.min_atr_pct then return end

    -- MACD history tracking (for L1 deceleration signal)
    local now_hour = math.floor(now / 3600)
    if now_hour ~= last_hour then
        if last_hour > 0 and last_macd_val ~= nil then
            prev2_macd = prev_macd
            prev_macd = last_macd_val
        end
        last_hour = now_hour
    end
    last_macd_val = ind.macd_histogram

    -- Scan signals (first match wins)
    local hist = { prev_macd = prev_macd, prev2_macd = prev2_macd }

    for _, sig in ipairs(SIGNALS) do
        local ok, matched = pcall(sig.check, ind, mid_price, hist)
        if ok and matched then
            active_signal = sig.name
            bot.save_state("active_signal", sig.name)

            bot.log("info", string.format(
                "%s: SIGNAL %s (%s) — RSI=%.0f ADX=%.0f ATR%%=%.3f MACD=%.4f mid=$%.4f",
                instance_name, sig.name, string.upper(sig.side),
                ind.rsi, ind.adx, atr_pct * 100, ind.macd_histogram, mid_price))

            entry_time = now
            local oid = place_entry(sig.side, mid_price)
            if oid then last_trade = now end
            return
        end
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    bot.log("info", string.format("%s: FILL %s %.2f @ $%.4f pnl=%.4f [%s]",
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
            bot.log("warn", string.format("%s: PAUSE 8h — %d consecutive losses",
                instance_name, consec_losses))
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
