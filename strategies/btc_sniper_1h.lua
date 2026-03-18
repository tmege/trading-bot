--[[
  Sniper 1h — Ultra-selective high-conviction strategy (BTC only)

  Philosophy: Trade like a sniper, not a machine gun.
  ~1.2 trades/month, maximum conviction, aggressive sizing.

  Grid-searched on 424k+ BTC 5m candles (8.6 years).
  x7 leverage + 90% equity = 630% exposure per trade.

  BTC (2 signals):
    L1: RSI>65 + LowVol(ATR<0.4%) + MACD decel   TP 2.0%/SL 2.0%
    S1: RSI<30 + MACD<0 + LowVol(ATR<0.4%)        TP 2.0%/SL 2.0%

  Backtest results (730j, x7, 90% equity):
    BTC: +275.2%, 28 trades, 75% WR, DD 20.7%, PF 5.39, ~11.3%/month

  Fees: 0.06% round-trip included in all EV calculations.
]]

-- Configuration
local config = {
    coin          = COIN or "BTC",

    -- Aggressive sizing (sniper = few shots, big positions)
    equity_pct    = 0.90,          -- 90% equity per trade
    leverage      = 7,             -- x7 leverage
    max_size      = 9999.0,        -- no hard cap (leverage is the limit)
    entry_size    = 100.0,         -- fallback minimum

    -- Patient timing
    check_sec     = 60,
    cooldown_sec  = 14400,         -- 4h between trades (signals are rare enough)
    max_hold_sec  = 172800,         -- 48h max hold

    -- Guard
    min_atr_pct   = 0.001,

    enabled       = true,
}

-- Signal Definitions: {name, side, tp, sl, check(ind, mid, hist)}
local COIN_SIGNALS = {}

COIN_SIGNALS["BTC"] = {
    -- LONG L1: Momentum haussier + faible volatilite + MACD decelerant
    { name = "L1_momentum_calm", side = "long", tp = 2.0, sl = 2.0,
      check = function(ind, mid, h)
          return ind.rsi > 65
             and (ind.atr / mid) < 0.004
             and h.prev_macd ~= nil and h.prev2_macd ~= nil
             and ind.macd_histogram < h.prev_macd
             and h.prev_macd < h.prev2_macd
      end },

    -- SHORT S1: Momentum baissier + faible volatilite
    { name = "S1_bear_momentum", side = "short", tp = 2.0, sl = 2.0,
      check = function(ind, mid, h)
          return ind.rsi < 30
             and ind.macd_histogram < 0
             and (ind.atr / mid) < 0.004
      end },
}

-- Instance Setup
local signals = COIN_SIGNALS[config.coin:upper()] or COIN_SIGNALS["BTC"]
local instance_name = "sniper_1h_" .. config.coin:lower()

-- Position sizing with drawdown guard
local peak_equity    = 0
local consec_losses  = 0

local function get_trade_size()
    local acct = bot.get_account_value()
    if not acct or acct <= 0 then return config.entry_size end

    -- Track peak equity for drawdown guard
    if acct > peak_equity then peak_equity = acct end

    -- Drawdown guard: reduce sizing after consecutive losses
    local dd_mult = 1.0
    if consec_losses >= 3 then
        dd_mult = 0.25     -- 3+ losses: quarter size
    elseif consec_losses >= 2 then
        dd_mult = 0.50     -- 2 losses: half size
    end

    -- Also reduce if in significant drawdown from peak
    if peak_equity > 0 then
        local dd_pct = (peak_equity - acct) / peak_equity * 100
        if dd_pct > 20 then
            dd_mult = 0    -- >20% DD: stop trading
        elseif dd_pct > 15 then
            dd_mult = math.min(dd_mult, 0.25)
        elseif dd_pct > 10 then
            dd_mult = math.min(dd_mult, 0.50)
        end
    end

    if dd_mult == 0 then return 0 end

    local size = acct * config.equity_pct * config.leverage * dd_mult
    return math.min(size, config.max_size)
end

-- State
local last_check     = 0
local last_trade     = 0
local in_position    = false
local position_side  = nil
local entry_price    = 0
local entry_time     = 0
local sl_oid         = nil
local tp_oid         = nil
local entry_oid      = nil
local entry_placed_at = 0
local ENTRY_TIMEOUT  = 90
local trade_count    = 0
local win_count      = 0

-- Per-signal TP/SL for active position
local active_tp      = 0
local active_sl      = 0
local active_signal  = ""

-- MACD history for cross/deceleration detection
local last_hour      = 0
local last_macd_val  = nil
local prev_macd      = nil
local prev2_macd     = nil

-- Helpers
local function place_entry(side, mid)
    if in_position then return nil end

    if entry_oid then
        bot.cancel(config.coin, entry_oid)
        entry_oid = nil
    end

    local trade_usd = get_trade_size()
    if trade_usd <= 0 then
        bot.log("warn", string.format("%s: SKIPPED — drawdown guard active", instance_name))
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
        bot.log("info", string.format("%s: ENTRY %s @ $%.2f ($%.0f, x%d)",
            instance_name, string.upper(side), price, trade_usd, config.leverage))
        return oid
    end
    return nil
end

local function place_sl_tp(fill_price, side, pos_size, tp_pct, sl_pct)
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

    bot.log("info", string.format("%s: SL=$%.2f (-%.1f%%), TP=$%.2f (+%.1f%%)",
        instance_name, sl_price, sl_pct, tp_price, tp_pct))
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
       and ind.adx and ind.ema_12 and ind.ema_26
       and ind.sma_20 and ind.sma_50
       and ind.stoch_rsi_k and ind.plus_di and ind.minus_di
       and ind.bb_lower and ind.bb_middle
       and ind.obv and ind.obv_sma
end

-- Callbacks
function on_init()
    if not COIN_SIGNALS[config.coin:upper()] then
        bot.log("warn", string.format("%s: coin %s not supported (BTC only)",
            instance_name, config.coin))
        config.enabled = false
    end

    bot.log("info", string.format(
        "%s: Sniper 1h [%d signals, EQ=%.0f%%, x%d, max=$%.0f]",
        instance_name, #signals, config.equity_pct * 100, config.leverage, config.max_size))

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

    -- Restore active signal TP/SL
    local stp = bot.load_state("active_tp")
    if stp then active_tp = tonumber(stp) or 0 end
    local ssl = bot.load_state("active_sl")
    if ssl then active_sl = tonumber(ssl) or 0 end
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
            if active_tp == 0 then active_tp = 3.0 end
            if active_sl == 0 then active_sl = 3.0 end
            place_sl_tp(entry_price, position_side, math.abs(pos.size), active_tp, active_sl)
            bot.log("info", string.format("%s: restored %s @ $%.2f [%s TP=%.1f%% SL=%.1f%%]",
                instance_name, position_side, entry_price, active_signal, active_tp, active_sl))
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

    -- Global cooldown (48h between trades on same coin)
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

    -- Get 1h indicators (50 candles lookback)
    local ind = bot.get_indicators(config.coin, "1h", 50, mid_price)
    if not ind then return end
    if not indicators_valid(ind) then return end

    -- Min volatility guard
    local atr_pct = ind.atr / mid_price
    if atr_pct < config.min_atr_pct then return end

    -- MACD history tracking (for ETH L1 deceleration signal)
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

    for _, sig in ipairs(signals) do
        local ok, matched = pcall(sig.check, ind, mid_price, hist)
        if ok and matched then
            bot.log("info", string.format(
                "%s: SIGNAL %s (%s) — RSI=%.0f ADX=%.0f ATR%%=%.3f MACD=%.4f",
                instance_name, sig.name, string.upper(sig.side),
                ind.rsi, ind.adx, atr_pct * 100, ind.macd_histogram))

            active_tp = sig.tp
            active_sl = sig.sl
            active_signal = sig.name
            bot.save_state("active_tp", tostring(sig.tp))
            bot.save_state("active_sl", tostring(sig.sl))
            bot.save_state("active_signal", sig.name)

            entry_time = now
            local oid = place_entry(sig.side, mid_price)
            if oid then last_trade = now end
            return
        end
    end
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
            place_sl_tp(entry_price, position_side, math.abs(pos.size), active_tp, active_sl)
        end
        return
    end

    -- Exit fill
    last_trade = bot.time()
    trade_count = trade_count + 1
    if fill.closed_pnl > 0 then
        win_count = win_count + 1
        consec_losses = 0
    else
        consec_losses = consec_losses + 1
    end
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))
    bot.save_state("consec_losses", tostring(consec_losses))

    bot.log("info", string.format(
        "%s: EXIT pnl=%.4f [%s TP=%.1f%% SL=%.1f%%] (W/L: %d/%d = %.0f%%, consec_loss=%d)",
        instance_name, fill.closed_pnl, active_signal, active_tp, active_sl,
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
    bot.save_state("active_tp", tostring(active_tp))
    bot.save_state("active_sl", tostring(active_sl))
    bot.save_state("active_signal", active_signal or "")

    local acct = bot.get_account_value()
    if acct and acct > peak_equity then peak_equity = acct end
    bot.save_state("peak_equity", tostring(peak_equity))
end
