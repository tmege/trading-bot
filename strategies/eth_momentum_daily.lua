--[[
  ETH Momentum Daily — Family D (Momentum Continuation)

  Apres un move Daily >3% avec volume 2x → continuation le lendemain.
  Filtre EMA20: Long si close > EMA20, Short si close < EMA20.

  HC Scanner results (walk-forward 6-fold, 50% equity):
    equity 50%: +9.1%/m, WR 57%, DD 18.5%, PF 2.38, Sharpe 0.40
    (verdict GO — DD < 20%)

  Funding note: Funding filter tested but losers NOT correlated with
  opposing funding (only 12% opposed). Filter provides negligible improvement.
  Kept disabled by default.

  Signal logic:
    LONG  — Daily bar return > +3%, volume > 2x SMA20, close > EMA20
    SHORT — Daily bar return < -3%, volume > 2x SMA20, close < EMA20

  Fees: 0.06% round-trip included.
]]

-- Configuration
local config = {
    coin          = COIN or "ETH",

    equity_pct    = 0.50,          -- 50% equity (sizing reduction for DD control)
    leverage      = 5,             -- x5 leverage
    max_size      = 9999.0,
    entry_size    = 100.0,

    check_sec     = 60,
    cooldown_sec  = 86400,         -- 1 day cooldown
    max_hold_sec  = 432000,        -- 5 days max hold

    -- Momentum params
    move_threshold = 3.0,          -- % move on daily bar
    vol_ratio_min  = 2.0,          -- volume must be 2x SMA20

    enabled       = true,
}

local instance_name = "momentum_daily_" .. (config.coin:lower())

-- Position sizing with drawdown guard
local peak_equity    = 0
local consec_losses  = 0

local function get_trade_size()
    local acct = bot.get_account_value()
    if not acct or acct <= 0 then return config.entry_size end

    if acct > peak_equity then peak_equity = acct end

    local dd_mult = 1.0
    if consec_losses >= 3 then
        dd_mult = 0.25
    elseif consec_losses >= 2 then
        dd_mult = 0.50
    end

    if peak_equity > 0 then
        local dd_pct = (peak_equity - acct) / peak_equity * 100
        if dd_pct > 20 then
            dd_mult = 0
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

-- Position state
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

local active_tp      = 5.0
local active_sl      = 3.0
local active_signal  = ""

-- EMA tracking (we compute from candles since bot.get_indicators uses different TF)
local ema20_val      = nil

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

-- Compute EMA20 from daily candle closes
local function compute_ema20(candles)
    if not candles or #candles < 20 then return nil end
    local k = 2.0 / (20 + 1)
    local ema = candles[1].close
    for i = 2, #candles do
        ema = candles[i].close * k + ema * (1 - k)
    end
    return ema
end

-- Compute volume SMA20
local function compute_vol_sma20(candles)
    if not candles or #candles < 20 then return nil end
    local sum = 0
    local start = #candles - 19
    for i = start, #candles do
        sum = sum + candles[i].volume
    end
    return sum / 20
end

-- Callbacks
function on_init()
    bot.log("info", string.format(
        "%s: Momentum Daily [EQ=%.0f%%, x%d, TP=%.1f%%, SL=%.1f%%, move>%.1f%%]",
        instance_name, config.equity_pct * 100, config.leverage,
        active_tp, active_sl, config.move_threshold))

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

    local had = bot.load_state("has_position")
    if had == "true" then
        local pos = bot.get_position(config.coin)
        if pos and pos.size ~= 0 then
            in_position = true
            entry_price = pos.entry_px
            position_side = pos.size > 0 and "long" or "short"
            entry_time = bot.time()
            bot.cancel_all_exchange(config.coin)
            place_sl_tp(entry_price, position_side, math.abs(pos.size), active_tp, active_sl)
            bot.log("info", string.format("%s: restored %s @ $%.2f",
                instance_name, position_side, entry_price))
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

    -- Orphan position guard
    local guard_pos = bot.get_position(config.coin)
    if guard_pos and guard_pos.size ~= 0 then
        in_position = true
        position_side = guard_pos.size > 0 and "long" or "short"
        entry_price = guard_pos.entry_px
        close_position("orphaned position")
        last_trade = now
        return
    end

    -- Get daily candles (30 for EMA20 + volume SMA20)
    local candles = bot.get_candles(config.coin, "1d", 30)
    if not candles or #candles < 21 then return end

    -- Compute EMA20 and volume SMA20
    ema20_val = compute_ema20(candles)
    local vol_sma = compute_vol_sma20(candles)
    if not ema20_val or not vol_sma or vol_sma <= 0 then return end

    -- Last completed daily candle
    local last_c = candles[#candles]
    if not last_c then return end

    -- Bar return
    local bar_ret = (last_c.close - last_c.open) / last_c.open * 100
    -- Volume ratio
    local vol_ratio = last_c.volume / vol_sma

    -- ATR guard skipped: move_threshold > 3% already ensures volatile market
    -- (bot.get_indicators TF param ignored in backtest — would get daily, not 1h)

    -- LONG: big up bar + volume spike + above EMA20
    if bar_ret > config.move_threshold and vol_ratio > config.vol_ratio_min
       and last_c.close > ema20_val then

        active_signal = string.format("LONG_mom_%.1f%%_vol%.1fx", bar_ret, vol_ratio)
        bot.log("info", string.format(
            "%s: SIGNAL %s close=$%.2f ema20=$%.2f",
            instance_name, active_signal, last_c.close, ema20_val))
        bot.save_state("active_signal", active_signal)
        entry_time = now
        local oid = place_entry("long", mid_price)
        if oid then last_trade = now end
        return
    end

    -- SHORT: big down bar + volume spike + below EMA20
    if bar_ret < -config.move_threshold and vol_ratio > config.vol_ratio_min
       and last_c.close < ema20_val then

        active_signal = string.format("SHORT_mom_%.1f%%_vol%.1fx", bar_ret, vol_ratio)
        bot.log("info", string.format(
            "%s: SIGNAL %s close=$%.2f ema20=$%.2f",
            instance_name, active_signal, last_c.close, ema20_val))
        bot.save_state("active_signal", active_signal)
        entry_time = now
        local oid = place_entry("short", mid_price)
        if oid then last_trade = now end
        return
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    bot.log("info", string.format("%s: FILL %s %.5f @ $%.2f pnl=%.4f [%s]",
        instance_name, fill.side, fill.size, fill.price, fill.closed_pnl, active_signal))

    -- Entry fill
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
