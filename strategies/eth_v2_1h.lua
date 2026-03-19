--[[
  ETH v2 — ADX/MFI multi-signal scanner (1h)

  Signals from 644k-eval scan, walk-forward validated (70/30):
    L1: RSI>70 + bullish_engulf    → LONG  TP 2.5%/SL 2.0% (OOS: 75.9% WR, +6.0%/m, DD 10.3%)
    S1: ADX>35 + MFI<20            → SHORT TP 2.5%/SL 2.5% (OOS: 70.2% WR, +8.1%/m, 57 trades)
    S2: hammer + MFI<20            → SHORT TP 4.0%/SL 3.0% (OOS: 63.2% WR, +6.7%/m)

  Config: x5 leverage, 20% equity = 100% exposure per trade.
  Fees: 0.06% round-trip included.
]]

-- Configuration
local config = {
    coin          = COIN or "ETH",
    equity_pct    = 0.20,
    leverage      = 5,
    max_size      = 9999.0,
    entry_size    = 100.0,
    check_sec     = 60,
    cooldown_sec  = 14400,         -- 4h
    max_hold_sec  = 172800,        -- 48h
    enabled       = true,
}

-- Signal Definitions
local SIGNALS = {
    { name = "L1_rsi70_bullengulf", side = "long", tp = 2.5, sl = 2.0,
      check = function(ind, mid, h)
          if ind.rsi <= 70 then return false end
          local c = h.candles
          if not c or #c < 2 then return false end
          local prev, curr = c[#c - 1], c[#c]
          local prev_red = prev.close < prev.open
          local curr_green = curr.close > curr.open
          local engulf = curr.close > prev.open and curr.open < prev.close
          return prev_red and curr_green and engulf
      end },

    { name = "S1_adx35_mfi20", side = "short", tp = 2.5, sl = 2.5,
      check = function(ind, mid, h)
          return ind.adx > 35 and ind.mfi < 20
      end },

    { name = "S2_hammer_mfi20", side = "short", tp = 4.0, sl = 3.0,
      check = function(ind, mid, h)
          if ind.mfi >= 20 then return false end
          local c = h.candles
          if not c or #c < 1 then return false end
          local last = c[#c]
          local body = math.abs(last.close - last.open)
          local range = last.high - last.low
          if range <= 0 then return false end
          local lower = math.min(last.open, last.close) - last.low
          return lower > 2 * body and body / range < 0.3
      end },
}

local instance_name = "eth_v2_1h"

-- Position sizing with drawdown guard
local peak_equity    = 0
local consec_losses  = 0

local function get_trade_size()
    local acct = bot.get_account_value()
    if not acct or acct <= 0 then return config.entry_size end
    if acct > peak_equity then peak_equity = acct end
    local dd_mult = 1.0
    if consec_losses >= 3 then dd_mult = 0.25
    elseif consec_losses >= 2 then dd_mult = 0.50 end
    if peak_equity > 0 then
        local dd_pct = (peak_equity - acct) / peak_equity * 100
        if dd_pct > 20 then dd_mult = 0
        elseif dd_pct > 15 then dd_mult = math.min(dd_mult, 0.25)
        elseif dd_pct > 10 then dd_mult = math.min(dd_mult, 0.50) end
    end
    if dd_mult == 0 then return 0 end
    return math.min(acct * config.equity_pct * config.leverage * dd_mult, config.max_size)
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
local active_tp      = 0
local active_sl      = 0
local active_signal  = ""

-- Helpers
local function place_entry(side, mid)
    if in_position then return nil end
    if entry_oid then
        bot.cancel(config.coin, entry_oid)
        entry_oid = nil
    end
    local trade_usd = get_trade_size()
    if trade_usd <= 0 then
        bot.log("warn", string.format("%s: SKIPPED — drawdown guard", instance_name))
        return nil
    end
    local price = side == "long" and mid * 0.9998 or mid * 1.0002
    local size = trade_usd / mid
    local order_side = side == "long" and "buy" or "sell"
    local oid = bot.place_limit(config.coin, order_side, price, size, { tif = "alo" })
    if oid then
        entry_oid = oid
        entry_placed_at = bot.time()
        bot.log("info", string.format("%s: ENTRY %s @ $%.2f ($%.0f, x%d)",
            instance_name, string.upper(side), price, trade_usd, config.leverage))
    end
    return oid
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

-- Callbacks
function on_init()
    bot.log("info", string.format("%s: v2 [%d signals, EQ=%.0f%%, x%d]",
        instance_name, #SIGNALS, config.equity_pct * 100, config.leverage))
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
            if active_tp == 0 then active_tp = 2.5 end
            if active_sl == 0 then active_sl = 2.0 end
            place_sl_tp(entry_price, position_side, math.abs(pos.size), active_tp, active_sl)
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

    if in_position and now - entry_time > config.max_hold_sec then
        close_position(string.format("max hold %ds", config.max_hold_sec))
        last_trade = now
        return
    end
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
    if entry_oid then
        if now - entry_placed_at > ENTRY_TIMEOUT then
            bot.cancel(config.coin, entry_oid)
            entry_oid = nil
        end
        return
    end
    if now - last_trade < config.cooldown_sec then return end

    local guard_pos = bot.get_position(config.coin)
    if guard_pos and guard_pos.size ~= 0 then
        in_position = true
        position_side = guard_pos.size > 0 and "long" or "short"
        entry_price = guard_pos.entry_px
        close_position("orphaned position")
        last_trade = now
        return
    end

    local ind = bot.get_indicators(config.coin, "1h", 0, mid_price)
    if not ind then return end
    if not ind.rsi or not ind.atr or not ind.mfi or not ind.adx then return end

    local candles = bot.get_candles(config.coin, "1h", 5)
    local hist = { candles = candles }

    for _, sig in ipairs(SIGNALS) do
        local ok, matched = pcall(sig.check, ind, mid_price, hist)
        if ok and matched then
            bot.log("info", string.format(
                "%s: SIGNAL %s (%s) RSI=%.0f MFI=%.0f ADX=%.0f",
                instance_name, sig.name, sig.side,
                ind.rsi, ind.mfi, ind.adx))
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
    bot.log("info", string.format("%s: EXIT pnl=%.4f [%s] (W/L: %d/%d=%.0f%%, consec=%d)",
        instance_name, fill.closed_pnl, active_signal,
        win_count, trade_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0, consec_losses))
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
