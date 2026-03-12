--[[
  BB Width Expansion Breakout Strategy — Generic Multi-Coin

  Breakout strategy on 1h timeframe.
  The COIN global is injected by the C engine before loading.
  Each instance runs independently with its own state.

  Logic:
  - Track BB width over time
  - Breakout: current bb_width > prev_bb_width * 1.2 (width expanding 20%+)
              AND prev_bb_width < 0.035 (was compressed)
  - Direction: price > bb_middle → LONG, price < bb_middle → SHORT
  - ATR-based exits: SL = 2x ATR, TP = 3x ATR
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = COIN or "ETH",
    leverage      = 7,

    -- Position sizing
    entry_size    = 40.0,        -- USD per trade
    equity_pct    = 0.10,        -- 10% of account per trade
    max_size      = 60.0,        -- hard cap

    -- ATR multipliers for exits
    sl_atr_mult   = 2.0,         -- stop loss = 2x ATR
    tp_atr_mult   = 3.0,         -- take profit = 3x ATR

    -- Timing
    check_sec     = 15,          -- check every 15s
    cooldown_sec  = 120,         -- 2 min between trades
    max_hold_sec  = 14400,       -- max hold 4h

    enabled       = true,
}

-- Instance name for logging and advisory
local instance_name = "bb_kc_squeeze_1h_" .. config.coin:lower()

local function get_trade_size()
    local acct = bot.get_account_value()
    if acct and acct > 0 then
        return acct * config.equity_pct
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
local prev_bb_width  = nil       -- previous tick BB width for expansion detection

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

local function place_sl_tp(fill_price, side, atr, pos_size)
    local sl_dist = atr * config.sl_atr_mult
    local tp_dist = atr * config.tp_atr_mult
    local sl_price, tp_price
    local size = pos_size

    if side == "long" then
        sl_price = fill_price - sl_dist
        tp_price = fill_price + tp_dist
        sl_oid = bot.place_trigger(config.coin, "sell", sl_price, size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "sell", tp_price, size, tp_price, "tp")
    else
        sl_price = fill_price + sl_dist
        tp_price = fill_price - tp_dist
        sl_oid = bot.place_trigger(config.coin, "buy", sl_price, size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "buy", tp_price, size, tp_price, "tp")
    end

    local sl_pct = (sl_dist / fill_price) * 100
    local tp_pct = (tp_dist / fill_price) * 100
    bot.log("info", string.format("%s: SL=$%.2f (%.1f%%), TP=$%.2f (+%.1f%%) [ATR=$%.2f]",
        instance_name, sl_price, sl_pct, tp_price, tp_pct, atr))
end

local function close_position(reason)
    if not in_position then return end

    if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
    if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end

    local pos = bot.get_position(config.coin)
    if not pos or pos.size == 0 then
        in_position = false
        position_side = nil
        entry_price = 0
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
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "%s: BB Width Expansion Breakout 1h, SL=%dx ATR, TP=%dx ATR, lever=%dx, check=%ds",
        instance_name, config.sl_atr_mult, config.tp_atr_mult, config.leverage, config.check_sec))

    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end

    local saved_trades = bot.load_state("trade_count")
    if saved_trades then trade_count = tonumber(saved_trades) or 0 end
    local saved_wins = bot.load_state("win_count")
    if saved_wins then win_count = tonumber(saved_wins) or 0 end

    -- Restore previous BB width
    local saved_bbw = bot.load_state("prev_bb_width")
    if saved_bbw then prev_bb_width = tonumber(saved_bbw) end

    -- Check existing position
    local pos = bot.get_position(config.coin)
    if pos and pos.size ~= 0 then
        in_position = true
        entry_price = pos.entry_px
        position_side = pos.size > 0 and "long" or "short"
        entry_time = bot.time()
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
            sl_oid = nil
            tp_oid = nil
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
        losing_streak = 0
    end

    -- Velocity guard (skip entry if price moved too fast)
    if not velocity_check(mid_price, now) then return end

    -- Get indicators on 1h timeframe
    local ind = bot.get_indicators(config.coin, "1h", 50, mid_price)
    if not ind then
        bot.log("warn", instance_name .. ": get_indicators returned nil")
        return
    end

    local bb_width  = ind.bb_width
    local bb_mid    = ind.bb_middle
    local atr       = ind.atr

    if not bb_width or not bb_mid or not atr then
        bot.log("warn", string.format("%s: missing fields — bb_width=%s bb_middle=%s atr=%s",
            instance_name, tostring(bb_width), tostring(bb_mid), tostring(atr)))
        return
    end

    bot.log("debug", string.format(
        "%s: %s=$%.2f bb_w=%.4f prev_bb_w=%s bb_mid=%.2f ATR=%.2f",
        instance_name, config.coin, mid_price,
        bb_width, tostring(prev_bb_width), bb_mid, atr))

    -- Detect BB width expansion breakout:
    -- current bb_width > prev * 1.2 (expanding 20%+) AND prev was compressed (< 0.035)
    if prev_bb_width and prev_bb_width < 0.035 and bb_width > prev_bb_width * 1.2 then
        -- Direction: price vs bb_middle
        if mid_price > bb_mid then
            bot.log("info", string.format(
                "%s: BB EXPANSION BREAKOUT LONG — bb_w=%.4f (prev=%.4f, +%.0f%%) price > bb_mid",
                instance_name, bb_width, prev_bb_width, (bb_width / prev_bb_width - 1) * 100))
            local oid = place_entry("long", mid_price)
            if oid then
                last_trade = now
                entry_time = now
                bot.save_state("entry_atr", tostring(atr))
            end
        elseif mid_price < bb_mid then
            bot.log("info", string.format(
                "%s: BB EXPANSION BREAKOUT SHORT — bb_w=%.4f (prev=%.4f, +%.0f%%) price < bb_mid",
                instance_name, bb_width, prev_bb_width, (bb_width / prev_bb_width - 1) * 100))
            local oid = place_entry("short", mid_price)
            if oid then
                last_trade = now
                entry_time = now
                bot.save_state("entry_atr", tostring(atr))
            end
        end
    end

    -- Update previous BB width
    prev_bb_width = bb_width
    bot.save_state("prev_bb_width", tostring(bb_width))
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    bot.log("info", string.format("%s: FILL %s %.5f @ $%.2f pnl=%.4f fee=%.4f",
        instance_name, fill.side, fill.size, fill.price, fill.closed_pnl, fill.fee))

    -- Entry fill
    if not in_position then
        entry_oid = nil
        entry_placed_at = 0
        in_position = true
        entry_price = fill.price
        position_side = fill.side == "buy" and "long" or "short"

        -- Retrieve ATR saved at entry time
        local saved_atr = bot.load_state("entry_atr")
        local atr = saved_atr and tonumber(saved_atr) or (fill.price * 0.02)
        place_sl_tp(fill.price, position_side, atr, fill.size)
        return
    end

    -- Exit fill
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

    if fill.oid == sl_oid then
        bot.log("info", string.format("%s: SL hit — pnl=%.4f (win rate: %d/%d = %.0f%%)",
            instance_name, fill.closed_pnl, win_count, trade_count,
            trade_count > 0 and (win_count / trade_count * 100) or 0))
        if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end
        sl_oid = nil
    elseif fill.oid == tp_oid then
        bot.log("info", string.format("%s: TP hit — pnl=%.4f (win rate: %d/%d = %.0f%%)",
            instance_name, fill.closed_pnl, win_count, trade_count,
            trade_count > 0 and (win_count / trade_count * 100) or 0))
        if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
        tp_oid = nil
    end

    in_position = false
    position_side = nil
    entry_price = 0
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
        if sl_oid then bot.cancel(config.coin, sl_oid); sl_oid = nil end
        if tp_oid then bot.cancel(config.coin, tp_oid); tp_oid = nil end
    end
end
function on_shutdown()
    bot.log("info", string.format("%s: shutdown — %d trades, %d wins (%.0f%%)",
        instance_name, trade_count, win_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0))
    bot.save_state("enabled", tostring(config.enabled))
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))
    bot.save_state("prev_bb_width", tostring(prev_bb_width or 0))

    if in_position then
        bot.log("info", instance_name .. ": position open, SL/TP on exchange")
    end
end
