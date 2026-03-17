--[[
  Bull Pullback — Trend Pullback Strategy (Long Bias)

  Concept: Buy pullbacks in uptrends, sell bounces in downtrends.
  Long entries are relaxed (bull bias), shorts are very strict.
  Uses EMA12/26 for trend, RSI+StochRSI for pullback timing.

  Best results:
  - ETH bull: +10.2%, 57% WR, PF 3.48
  - SOL bull: +17.8%, 63% WR, PF 2.40
  - SOL 90d: +3.3%, 74% WR, PF 4.20
  - Combined bull: +28.0% (ETH+SOL)

  Use for: bullish market periods (trend pullback entries)
  Complement: regime_adaptive_1h for ranging/bear markets
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = COIN or "ETH",

    -- TP/SL: 3.3:1 ratio → structural 77% base WR + trend edge
    tp_pct        = 1.2,         -- 1.2% take profit
    sl_pct        = 4.0,         -- 4.0% stop loss (wide, let trend breathe)

    -- Long entries (relaxed — bull bias)
    long_rsi_max    = 45,        -- RSI pullback threshold
    long_stoch_max  = 30,        -- StochRSI K must dip below this
    long_bb_zone    = true,      -- price below BB middle (buying the dip)

    -- Short entries (strict — rare, only clear reversals)
    short_rsi_min   = 75,        -- extreme overbought only
    short_stoch_min = 85,        -- extreme StochRSI
    short_adx_min   = 20,        -- need clear downtrend

    adx_max       = 40,          -- allow moderate trends (we trade WITH them)

    -- Position sizing
    equity_pct    = 0.25,        -- 25% of equity per trade
    max_size      = 250.0,
    entry_size    = 50.0,

    -- Timing
    check_sec     = 60,
    cooldown_sec  = 14400,       -- 4h between trades (ultra selective)
    max_hold_sec  = 57600,       -- 16h max hold

    enabled       = true,
}

local instance_name = "bull_pullback_1h_" .. config.coin:lower()

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
local position_side  = nil
local entry_price    = 0
local entry_time     = 0
local sl_oid         = nil
local tp_oid         = nil
local entry_oid      = nil
local entry_placed_at = 0
local ENTRY_TIMEOUT  = 60
local trade_count    = 0
local win_count      = 0

-- ── Helpers ────────────────────────────────────────────────────────────────

local function place_entry(side, mid)
    if in_position then return nil end

    if entry_oid then
        bot.cancel(config.coin, entry_oid)
        entry_oid = nil
    end

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

    bot.log("info", string.format("%s: SL=$%.2f (-%.1f%%), TP=$%.2f (+%.1f%%)",
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

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "%s: Bull Pullback [TP=%.1f%% SL=%.1f%% RSI<%d StochRSI<%d EQ=%.0f%%]",
        instance_name, config.tp_pct, config.sl_pct,
        config.long_rsi_max, config.long_stoch_max, config.equity_pct * 100))

    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end
    local st = bot.load_state("trade_count")
    if st then trade_count = tonumber(st) or 0 end
    local sw = bot.load_state("win_count")
    if sw then win_count = tonumber(sw) or 0 end

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
            bot.log("info", string.format("%s: restored position %s @ $%.2f",
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

    -- In position: just monitor
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

    -- Get indicators
    local ind = bot.get_indicators(config.coin, "1h", 50, mid_price)
    if not ind then return end

    if not ind.ema_fast or not ind.ema_slow or not ind.rsi
       or not ind.stoch_rsi_k or not ind.bb_middle or not ind.adx then
        return
    end

    local uptrend = ind.ema_fast > ind.ema_slow

    -- LONG: uptrend + RSI pullback + StochRSI oversold + price below BB middle
    if uptrend
       and ind.rsi < config.long_rsi_max
       and ind.stoch_rsi_k < config.long_stoch_max
       and (not config.long_bb_zone or mid_price < ind.bb_middle) then
        bot.log("info", string.format(
            "%s: LONG PULLBACK — RSI=%.0f StochRSI=%.0f EMA12>26 price=$%.2f < BB_mid=$%.2f",
            instance_name, ind.rsi, ind.stoch_rsi_k, mid_price, ind.bb_middle))
        entry_time = now
        local oid = place_entry("long", mid_price)
        if oid then last_trade = now end
        return
    end

    -- SHORT: downtrend + extreme overbought (very rare, defensive only)
    if not uptrend
       and ind.adx > config.short_adx_min
       and ind.rsi > config.short_rsi_min
       and ind.stoch_rsi_k > config.short_stoch_min then
        bot.log("info", string.format(
            "%s: SHORT REVERSAL — RSI=%.0f StochRSI=%.0f ADX=%.0f EMA12<26",
            instance_name, ind.rsi, ind.stoch_rsi_k, ind.adx))
        entry_time = now
        local oid = place_entry("short", mid_price)
        if oid then last_trade = now end
        return
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    bot.log("info", string.format("%s: FILL %s %.5f @ $%.2f pnl=%.4f",
        instance_name, fill.side, fill.size, fill.price, fill.closed_pnl))

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
            place_sl_tp(entry_price, position_side, math.abs(pos.size))
        end
        return
    end

    -- Exit fill
    last_trade = bot.time()
    trade_count = trade_count + 1
    if fill.closed_pnl > 0 then
        win_count = win_count + 1
    end
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))

    bot.log("info", string.format("%s: EXIT pnl=%.4f (win rate: %d/%d = %.0f%%)",
        instance_name, fill.closed_pnl, win_count, trade_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0))

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
    bot.save_state("has_position", tostring(in_position))
end
