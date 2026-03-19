--[[
  BTC Swing Levels 4h — Family A (PWH/PWL/PMH/PML)

  Rebond/rejet sur niveaux cles hebdo/mensuels avec confirmation de bougie.
  Filtrage funding rate (strong): Long si FR<0, Short si FR>0.

  HC Scanner results (walk-forward 6-fold, 50% equity):
    x7 + Filter 2 (strong): +7.4%/m, WR 50%, DD 4.0%, PF 5.55, Sharpe 0.49
    x7 baseline:            +5.2%/m, WR 48%, DD 7.3%, PF 3.63, Sharpe 0.36

  Signal logic:
    LONG  — price touches PWL or PML (buffer 0.3%), bullish candle, RSI<45
    SHORT — price touches PWH or PMH (buffer 0.3%), bearish candle, RSI>55
    + Funding filter: Long only if last FR < 0, Short only if last FR > 0

  Fees: 0.06% round-trip included.
]]

-- Configuration
local config = {
    coin          = COIN or "BTC",

    equity_pct    = 0.50,          -- 50% equity per trade
    leverage      = 7,             -- x7 leverage
    max_size      = 9999.0,
    entry_size    = 100.0,

    check_sec     = 60,
    cooldown_sec  = 14400,         -- 4h cooldown (1 bar)
    max_hold_sec  = 172800,        -- 48h max hold (12 bars 4h)

    -- Swing level params
    buffer_pct    = 0.003,         -- 0.3% buffer around levels
    body_ratio    = 0.5,           -- confirmation candle body ratio
    rsi_long_max  = 45,            -- RSI < 45 for long
    rsi_short_min = 55,            -- RSI > 55 for short

    -- Funding filter
    use_funding   = true,          -- enable funding rate filter

    min_atr_pct   = 0.001,
    enabled       = true,
}

local instance_name = "swing_levels_4h_" .. (config.coin:lower())

-- State: weekly/monthly high-low tracking
local prev_week_high  = nil
local prev_week_low   = nil
local prev_month_high = nil
local prev_month_low  = nil
local cur_week_high   = nil
local cur_week_low    = nil
local cur_month_high  = nil
local cur_month_low   = nil
local last_week_key   = nil
local last_month_key  = nil

-- Pure Lua timestamp decomposition (no os.date — not available in sandbox)
-- Howard Hinnant algorithm: https://howardhinnant.github.io/date_algorithms.html
local function ts_to_year_month(ts_sec)
    local z = math.floor(ts_sec / 86400) + 719468
    local era
    if z >= 0 then
        era = math.floor(z / 146097)
    else
        era = math.floor((z - 146096) / 146097)
    end
    local doe = z - era * 146097
    local yoe = math.floor((doe - math.floor(doe / 1460) + math.floor(doe / 36524)
                            - math.floor(doe / 146096)) / 365)
    local y = yoe + era * 400
    local doy = doe - (365 * yoe + math.floor(yoe / 4) - math.floor(yoe / 100))
    local mp = math.floor((5 * doy + 2) / 153)
    local m = mp + (mp < 10 and 3 or -9)
    if m <= 2 then y = y + 1 end
    return y, m
end

local function ts_to_week_key(ts_sec)
    -- Epoch 1970-01-01 was Thursday. Shift +3 days so Monday=0.
    return math.floor((ts_sec + 259200) / 604800)
end

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

local active_tp      = 4.0
local active_sl      = 1.0
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

-- Swing level computation from 4h candles (sandbox-safe, no os.date)
local function update_levels(candles)
    if not candles or #candles < 2 then return end

    -- Reset tracking state for full recompute from candle history
    last_week_key = nil
    last_month_key = nil
    cur_week_high = nil
    cur_week_low = nil
    cur_month_high = nil
    cur_month_low = nil
    prev_week_high = nil
    prev_week_low = nil
    prev_month_high = nil
    prev_month_low = nil

    for _, c in ipairs(candles) do
        local ts = c.time / 1000  -- candle time is ms
        local wk = ts_to_week_key(ts)
        local y, m = ts_to_year_month(ts)
        local mk = y * 100 + m   -- unique month key (e.g. 202603)

        -- Week tracking
        if last_week_key == nil then
            last_week_key = wk
            cur_week_high = c.high
            cur_week_low = c.low
        elseif wk ~= last_week_key then
            prev_week_high = cur_week_high
            prev_week_low = cur_week_low
            cur_week_high = c.high
            cur_week_low = c.low
            last_week_key = wk
        else
            if c.high > cur_week_high then cur_week_high = c.high end
            if c.low < cur_week_low then cur_week_low = c.low end
        end

        -- Month tracking
        if last_month_key == nil then
            last_month_key = mk
            cur_month_high = c.high
            cur_month_low = c.low
        elseif mk ~= last_month_key then
            prev_month_high = cur_month_high
            prev_month_low = cur_month_low
            cur_month_high = c.high
            cur_month_low = c.low
            last_month_key = mk
        else
            if c.high > cur_month_high then cur_month_high = c.high end
            if c.low < cur_month_low then cur_month_low = c.low end
        end
    end
end

-- Check if price is near a support level (PWL/PML)
local function near_support(low_price)
    local buf = config.buffer_pct
    for _, level in ipairs({prev_week_low, prev_month_low}) do
        if level and level > 0 then
            if math.abs(low_price - level) / level < buf then
                return true, level
            end
        end
    end
    return false, nil
end

-- Check if price is near a resistance level (PWH/PMH)
local function near_resistance(high_price)
    local buf = config.buffer_pct
    for _, level in ipairs({prev_week_high, prev_month_high}) do
        if level and level > 0 then
            if math.abs(high_price - level) / level < buf then
                return true, level
            end
        end
    end
    return false, nil
end

-- Callbacks
function on_init()
    bot.log("info", string.format(
        "%s: Swing Levels 4h [EQ=%.0f%%, x%d, TP=%.1f%%, SL=%.1f%%, buf=%.1f%%, funding=%s]",
        instance_name, config.equity_pct * 100, config.leverage,
        active_tp, active_sl, config.buffer_pct * 100,
        config.use_funding and "ON" or "OFF"))

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

    -- Get 4h candles for level computation + indicators
    local candles = bot.get_candles(config.coin, "4h", 200)
    if not candles or #candles < 50 then return end

    -- Update PWH/PWL/PMH/PML from candle history
    update_levels(candles)

    -- Need at least prev week levels
    if not prev_week_high or not prev_week_low then return end

    -- Get indicators
    local ind = bot.get_indicators(config.coin, "4h", 50, mid_price)
    if not ind then return end
    if not ind.rsi or not ind.atr then return end

    local atr_pct = ind.atr / mid_price
    if atr_pct < config.min_atr_pct then return end

    -- Get last candle for confirmation
    local last_c = candles[#candles]
    if not last_c then return end

    local body = last_c.close - last_c.open
    local bar_range = last_c.high - last_c.low
    if bar_range < 1e-10 then return end
    local body_r = math.abs(body) / bar_range
    local is_bullish = body > 0 and body_r > config.body_ratio
    local is_bearish = body < 0 and body_r > config.body_ratio

    -- Funding rate check (live only, nil in backtest)
    local fr_data = nil
    if config.use_funding then
        fr_data = bot.get_funding_rate(config.coin)
    end

    -- LONG signal: near support + bullish candle + RSI < 45
    local near_sup, sup_level = near_support(last_c.low)
    if near_sup and is_bullish and ind.rsi < config.rsi_long_max then
        -- Funding filter: skip if FR >= 0 (crowd is long → don't add)
        if config.use_funding and fr_data and fr_data.rate >= 0 then
            bot.log("debug", string.format("%s: LONG skipped — FR=%.4f%% >= 0",
                instance_name, fr_data.rate * 100))
        else
            active_signal = string.format("LONG_support_%.0f", sup_level)
            bot.log("info", string.format(
                "%s: SIGNAL %s RSI=%.0f ATR%%=%.3f lvl=$%.0f FR=%s",
                instance_name, active_signal, ind.rsi, atr_pct * 100, sup_level,
                fr_data and string.format("%.4f%%", fr_data.rate * 100) or "N/A"))
            bot.save_state("active_signal", active_signal)
            entry_time = now
            local oid = place_entry("long", mid_price)
            if oid then last_trade = now end
            return
        end
    end

    -- SHORT signal: near resistance + bearish candle + RSI > 55
    local near_res, res_level = near_resistance(last_c.high)
    if near_res and is_bearish and ind.rsi > config.rsi_short_min then
        -- Funding filter: skip if FR <= 0 (crowd is short → don't add)
        if config.use_funding and fr_data and fr_data.rate <= 0 then
            bot.log("debug", string.format("%s: SHORT skipped — FR=%.4f%% <= 0",
                instance_name, fr_data.rate * 100))
        else
            active_signal = string.format("SHORT_resist_%.0f", res_level)
            bot.log("info", string.format(
                "%s: SIGNAL %s RSI=%.0f ATR%%=%.3f lvl=$%.0f FR=%s",
                instance_name, active_signal, ind.rsi, atr_pct * 100, res_level,
                fr_data and string.format("%.4f%%", fr_data.rate * 100) or "N/A"))
            bot.save_state("active_signal", active_signal)
            entry_time = now
            local oid = place_entry("short", mid_price)
            if oid then last_trade = now end
            return
        end
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
