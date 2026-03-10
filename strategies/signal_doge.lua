--[[
  Signal Trading Strategy — DOGE (v2)

  Upgraded with:
  - Better R:R: TP +8%, SL -2%
  - Larger entry: $120 (with 5x leverage)
  - Faster cooldown: 30 min
  - Trailing stop: at +3% → SL to breakeven, at +5% → SL to +3%
  - Supports both long and short on sentiment spikes
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin              = "DOGE",
    leverage          = 5,

    -- Entry conditions
    sentiment_thresh  = 0.3,    -- minimum sentiment score to trigger
    volume_mult       = 1.5,    -- volume must be > avg * this
    volume_window     = 20,     -- candles to compute avg volume
    fg_min            = 15,     -- skip entry if Fear & Greed < this
    btc_drop_pct      = -3.0,   -- skip if BTC dropped > this % in 24h

    -- Position management
    entry_size        = 120.0,  -- USD notional per trade
    stop_loss_pct     = 2.0,    -- stop loss percentage
    take_profit_pct   = 8.0,    -- take profit percentage
    max_size_usd      = 150.0,  -- hard cap

    -- Trailing stop
    trail_activate_pct = 3.0,   -- activate trailing at +3%
    trail_step1_pct    = 3.0,   -- at +3%: move SL to breakeven
    trail_step2_pct    = 5.0,   -- at +5%: move SL to +3%
    trail_step2_sl_pct = 3.0,   -- SL level at step 2 (locks in +3%)

    -- Cooldown
    cooldown_sec      = 1800,   -- 30 minutes between entries

    -- Timing
    check_sec         = 30,     -- check signals every 30s

    enabled           = true,
}

-- ── Internal state ─────────────────────────────────────────────────────────

local last_entry_time = 0
local last_check_time = 0
local in_position     = false
local entry_price     = 0
local entry_side      = "buy"
local entry_oid       = nil
local sl_oid          = nil
local tp_oid          = nil
local btc_price_24h   = 0
local btc_price_now   = 0
local current_sl_pct  = 0      -- tracks where SL currently is (for trailing)
local peak_pnl_pct    = 0      -- peak unrealized P&L %

-- ── Helpers ────────────────────────────────────────────────────────────────

local function check_volume_spike()
    local candles = bot.get_candles(config.coin, "15m", config.volume_window + 5)
    if not candles or #candles < config.volume_window + 1 then
        return false, 0, 0
    end

    local sum = 0
    local start = #candles - config.volume_window
    if start < 1 then start = 1 end
    for i = start, #candles - 1 do
        sum = sum + candles[i].v
    end
    local avg_vol = sum / config.volume_window
    local current_vol = candles[#candles].v

    return current_vol > avg_vol * config.volume_mult, current_vol, avg_vol
end

local function check_macro_context()
    local fg = bot.get_fear_greed()
    if fg and fg.valid and fg.value < config.fg_min then
        bot.log("debug", string.format("signal: F&G=%d < %d, skipping",
            fg.value, config.fg_min))
        return false, "fear_greed_too_low"
    end

    local macro = bot.get_macro()
    if macro and macro.valid and macro.btc_price > 0 then
        btc_price_now = macro.btc_price
        if btc_price_24h > 0 then
            local btc_change = ((btc_price_now - btc_price_24h) / btc_price_24h) * 100
            if btc_change < config.btc_drop_pct then
                bot.log("debug", string.format("signal: BTC dropped %.1f%%, skipping",
                    btc_change))
                return false, "btc_dumping"
            end
        end
    end

    return true, "ok"
end

local function check_sentiment_signal()
    local sent = bot.get_sentiment()
    if not sent or not sent.valid then
        return false, 0
    end

    if sent.score >= config.sentiment_thresh then
        return true, sent.score
    end

    if sent.score <= -config.sentiment_thresh then
        return true, sent.score
    end

    return false, sent.score
end

local function place_entry(side, mid)
    local oid, err = bot.place_limit(config.coin, side, mid,
                                      config.entry_size / mid,
                                      { tif = "ioc" })
    if not oid then
        bot.log("warn", string.format("signal: entry failed: %s", err or "unknown"))
        return false
    end

    entry_oid = oid
    entry_side = side
    bot.log("info", string.format("signal: ENTRY %s DOGE @ $%.5f (IOC) oid=%d",
        side, mid, oid))
    return true
end

local function place_sl_tp(fill_price, side)
    local sl_price, tp_price

    if side == "buy" then
        sl_price = fill_price * (1 - config.stop_loss_pct / 100)
        tp_price = fill_price * (1 + config.take_profit_pct / 100)

        sl_oid = bot.place_trigger(config.coin, "sell", sl_price,
                                    config.entry_size / fill_price,
                                    sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "sell", tp_price,
                                    config.entry_size / fill_price,
                                    tp_price, "tp")
    else
        sl_price = fill_price * (1 + config.stop_loss_pct / 100)
        tp_price = fill_price * (1 - config.take_profit_pct / 100)

        sl_oid = bot.place_trigger(config.coin, "buy", sl_price,
                                    config.entry_size / fill_price,
                                    sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "buy", tp_price,
                                    config.entry_size / fill_price,
                                    tp_price, "tp")
    end

    current_sl_pct = -config.stop_loss_pct  -- initial SL is negative
    bot.log("info", string.format("signal: SL @ $%.5f (%.1f%%), TP @ $%.5f (+%.1f%%)",
        sl_price, -config.stop_loss_pct, tp_price, config.take_profit_pct))
end

local function move_stop_loss(new_sl_price)
    if sl_oid then
        bot.cancel(config.coin, sl_oid)
        sl_oid = nil
    end

    local sl_side = entry_side == "buy" and "sell" or "buy"
    local pos = bot.get_position(config.coin)
    local size = config.entry_size / entry_price
    if pos and pos.size ~= 0 then
        size = math.abs(pos.size)
    end

    sl_oid = bot.place_trigger(config.coin, sl_side, new_sl_price,
                                size, new_sl_price, "sl")
    bot.log("info", string.format("signal: trailing SL moved to $%.5f", new_sl_price))
end

local function update_trailing(mid)
    if not in_position or entry_price <= 0 then return end

    local pnl_pct
    if entry_side == "buy" then
        pnl_pct = ((mid - entry_price) / entry_price) * 100
    else
        pnl_pct = ((entry_price - mid) / entry_price) * 100
    end

    if pnl_pct > peak_pnl_pct then
        peak_pnl_pct = pnl_pct
    end

    -- Step 2: at +5%, lock in +3%
    if pnl_pct >= config.trail_step2_pct and current_sl_pct < config.trail_step2_sl_pct then
        local new_sl
        if entry_side == "buy" then
            new_sl = entry_price * (1 + config.trail_step2_sl_pct / 100)
        else
            new_sl = entry_price * (1 - config.trail_step2_sl_pct / 100)
        end
        move_stop_loss(new_sl)
        current_sl_pct = config.trail_step2_sl_pct
        bot.log("info", string.format("signal: TRAIL step 2 — locking +%.1f%% profit", config.trail_step2_sl_pct))
        return
    end

    -- Step 1: at +3%, move SL to breakeven
    if pnl_pct >= config.trail_step1_pct and current_sl_pct < 0 then
        local new_sl
        if entry_side == "buy" then
            new_sl = entry_price * 1.001  -- tiny profit to cover fees
        else
            new_sl = entry_price * 0.999
        end
        move_stop_loss(new_sl)
        current_sl_pct = 0
        bot.log("info", "signal: TRAIL step 1 — SL moved to breakeven")
    end
end

local function close_position()
    if not in_position then return end

    if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
    if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end

    local pos = bot.get_position(config.coin)
    if pos and pos.size ~= 0 then
        local mid = bot.get_mid_price(config.coin)
        if mid then
            local close_side = pos.size > 0 and "sell" or "buy"
            local close_size = math.abs(pos.size)
            bot.place_limit(config.coin, close_side, mid, close_size,
                           { tif = "ioc", reduce_only = true })
            bot.log("info", string.format("signal: closing position %s %.4f @ $%.5f",
                close_side, close_size, mid))
        end
    end

    in_position = false
    entry_price = 0
    current_sl_pct = 0
    peak_pnl_pct = 0
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "signal_doge v2: thresh=%.2f, SL=%.1f%%, TP=%.1f%%, trail=%s, cooldown=%ds, entry=$%.0f",
        config.sentiment_thresh, config.stop_loss_pct,
        config.take_profit_pct, "ON", config.cooldown_sec, config.entry_size))

    -- Restore state
    local saved_enabled = bot.load_state("enabled")
    if saved_enabled == "false" then config.enabled = false end
    local saved_thresh = bot.load_state("sentiment_thresh")
    if saved_thresh then config.sentiment_thresh = tonumber(saved_thresh) end

    -- Check existing position
    local pos = bot.get_position(config.coin)
    if pos and pos.size ~= 0 then
        in_position = true
        entry_price = pos.entry_px
        entry_side = pos.size > 0 and "buy" or "sell"
        bot.log("info", string.format("signal_doge: existing position %.4f @ $%.5f",
            pos.size, pos.entry_px))
    end

    local macro = bot.get_macro()
    if macro and macro.valid then
        btc_price_24h = macro.btc_price
    end
end

function on_tick(coin, mid_price)
    if coin ~= config.coin then return end

    -- Always update trailing stop if in position
    if in_position then
        update_trailing(mid_price)
    end

    if not config.enabled then return end

    local now = bot.time()

    if now - last_check_time < config.check_sec then return end
    last_check_time = now

    if in_position then
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            bot.log("info", "signal: position closed (SL/TP hit)")
            in_position = false
            entry_price = 0
            sl_oid = nil
            tp_oid = nil
            current_sl_pct = 0
            peak_pnl_pct = 0
        end
        return
    end

    -- Cooldown
    if now - last_entry_time < config.cooldown_sec then
        return
    end

    -- 1. Sentiment signal
    local has_sentiment, score = check_sentiment_signal()
    if not has_sentiment then return end

    -- 2. Volume spike
    local has_volume, cur_vol, avg_vol = check_volume_spike()
    if not has_volume then
        bot.log("debug", string.format("signal: sentiment=%.2f but no volume spike (%.0f vs avg %.0f)",
            score, cur_vol, avg_vol))
        return
    end

    -- 3. Macro context
    local macro_ok, reason = check_macro_context()
    if not macro_ok then
        bot.log("info", string.format("signal: sentiment=%.2f + volume OK, but macro blocked: %s",
            score, reason))
        return
    end

    -- ENTRY
    local side = score > 0 and "buy" or "sell"

    bot.log("info", string.format(
        "signal: TRIGGERED — sentiment=%.2f, vol=%.0f (%.1fx avg), side=%s",
        score, cur_vol, cur_vol / avg_vol, side))

    if place_entry(side, mid_price) then
        last_entry_time = now
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    bot.log("info", string.format("signal: FILL %s %.4f DOGE @ $%.5f pnl=%.4f fee=%.4f",
        fill.side, fill.size, fill.price, fill.closed_pnl, fill.fee))

    -- Entry fill?
    if fill.oid == entry_oid and not in_position then
        in_position = true
        entry_price = fill.price
        entry_oid = nil
        current_sl_pct = -config.stop_loss_pct
        peak_pnl_pct = 0

        place_sl_tp(fill.price, fill.side)
        return
    end

    -- SL fill?
    if fill.oid == sl_oid then
        bot.log("info", string.format("signal: STOP LOSS hit — pnl=%.4f (trail was at %.1f%%)",
            fill.closed_pnl, current_sl_pct))
        if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end
        in_position = false
        entry_price = 0
        sl_oid = nil
        current_sl_pct = 0
        peak_pnl_pct = 0
        return
    end

    -- TP fill?
    if fill.oid == tp_oid then
        bot.log("info", string.format("signal: TAKE PROFIT hit — pnl=%.4f", fill.closed_pnl))
        if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
        in_position = false
        entry_price = 0
        tp_oid = nil
        current_sl_pct = 0
        peak_pnl_pct = 0
        return
    end
end

function on_timer()
    if not config.enabled then return end
    if not in_position then return end

    local pos = bot.get_position(config.coin)
    if not pos or pos.size == 0 then
        bot.log("info", "signal: position disappeared, cleaning up")
        in_position = false
        entry_price = 0
        if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
        if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end
        current_sl_pct = 0
        peak_pnl_pct = 0
    end

    local macro = bot.get_macro()
    if macro and macro.valid and macro.btc_price > 0 then
        btc_price_now = macro.btc_price
    end
end

function on_advisory(json_str)
    bot.log("info", "signal_doge: advisory: " .. json_str)

    local new_thresh = json_str:match('"sentiment_thresh"%s*:%s*([%d%.]+)')
    local new_sl = json_str:match('"stop_loss_pct"%s*:%s*([%d%.]+)')
    local new_tp = json_str:match('"take_profit_pct"%s*:%s*([%d%.]+)')
    local new_size = json_str:match('"entry_size"%s*:%s*([%d%.]+)')
    local pause = json_str:match('"pause"%s*:%s*(true)')
    local resume = json_str:match('"pause"%s*:%s*(false)')

    if new_thresh then
        local v = tonumber(new_thresh)
        if v and v > 0 and v < 1.0 then
            config.sentiment_thresh = v
            bot.save_state("sentiment_thresh", tostring(v))
            bot.log("info", string.format("signal: advisory set thresh=%.2f", v))
        end
    end

    if new_sl then
        local v = tonumber(new_sl)
        if v and v > 0 and v <= 10.0 then
            config.stop_loss_pct = v
            bot.log("info", string.format("signal: advisory set SL=%.1f%%", v))
        end
    end

    if new_tp then
        local v = tonumber(new_tp)
        if v and v > 0 and v <= 20.0 then
            config.take_profit_pct = v
            bot.log("info", string.format("signal: advisory set TP=%.1f%%", v))
        end
    end

    if new_size then
        local v = tonumber(new_size)
        if v and v > 0 and v <= config.max_size_usd then
            config.entry_size = v
            bot.log("info", string.format("signal: advisory set size=$%.0f", v))
        end
    end

    if pause then
        config.enabled = false
        bot.save_state("enabled", "false")
        if in_position then close_position() end
        bot.log("warn", "signal: PAUSED by advisory")
    end

    if resume and not config.enabled then
        config.enabled = true
        bot.save_state("enabled", "true")
        bot.log("info", "signal: RESUMED by advisory")
    end
end

function on_shutdown()
    bot.log("info", "signal_doge: shutting down")
    bot.save_state("enabled", tostring(config.enabled))
    bot.save_state("sentiment_thresh", tostring(config.sentiment_thresh))

    if in_position then
        bot.log("info", "signal_doge: position open, SL/TP remain on exchange")
    end
end
