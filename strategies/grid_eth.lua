--[[
  Grid Trading Strategy — ETH (v2)

  Upgraded grid with:
  - 15 levels, tighter spacing, larger orders (0.008 ETH = ~$16-20/level)
  - ATR-based dynamic range (auto-adjusts to volatility)
  - Breakout protection: if ETH breaks out of grid range, switch to
    momentum mode (ride the trend) instead of just pausing
  - Auto-recenter: when breakout trade closes, rebuild grid around new price
  - 5x leverage
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin         = "ETH",
    n_levels     = 15,         -- grid levels
    order_size   = 0.008,      -- ETH per order (~$16-20)
    leverage     = 5,          -- 5x leverage
    check_sec    = 60,         -- integrity check interval
    enabled      = true,

    -- Dynamic range (ATR-based)
    atr_period   = 14,         -- ATR lookback (1h candles)
    atr_mult     = 2.5,        -- range = mid +/- ATR * mult
    range_min    = 100.0,      -- minimum range width ($)
    range_max    = 500.0,      -- maximum range width ($)
    recenter_cd  = 300,        -- cooldown between recenters (5 min)

    -- Breakout protection
    breakout_enabled  = true,
    breakout_size     = 0.015,     -- larger position for breakout (ETH)
    breakout_sl_pct   = 2.0,       -- tight stop loss on breakout
    breakout_tp_pct   = 6.0,       -- take profit on breakout
    trail_activate    = 3.0,       -- activate trailing stop at +3%
    trail_offset_pct  = 1.5,       -- trail 1.5% behind peak

    -- Safety
    max_drop_24h_pct  = 8.0,       -- force close if ETH drops > 8% in 24h
}

-- ── Internal state ─────────────────────────────────────────────────────────

local grid = {}               -- grid[i] = { price, buy_oid, sell_oid }
local range_low    = 0
local range_high   = 0
local last_check   = 0
local last_recenter = 0
local initialized  = false

-- Breakout state
local breakout_mode    = false     -- currently in breakout trade?
local breakout_side    = nil       -- "long" or "short"
local breakout_entry   = 0
local breakout_peak    = 0         -- highest/lowest since entry (for trailing)
local breakout_sl_oid  = nil
local breakout_tp_oid  = nil
local breakout_oid     = nil       -- entry order OID

-- 24h safety
local eth_price_24h    = 0
local price_history    = {}        -- circular buffer of recent prices

-- ── Helpers ────────────────────────────────────────────────────────────────

local function round_price(p, tick)
    return math.floor(p / tick + 0.5) * tick
end

local function compute_atr_range(mid)
    local indicators = bot.get_indicators(config.coin, "1h", 50)
    local atr = nil
    if indicators and indicators.atr then
        atr = indicators.atr
    end

    local half_range
    if atr and atr > 0 then
        half_range = atr * config.atr_mult
        -- Clamp to min/max
        if half_range * 2 < config.range_min then
            half_range = config.range_min / 2
        elseif half_range * 2 > config.range_max then
            half_range = config.range_max / 2
        end
    else
        -- Fallback: 2% of mid price on each side
        half_range = mid * 0.02
        bot.log("debug", "grid: ATR unavailable, using 2% fallback range")
    end

    return round_price(mid - half_range, 1.0), round_price(mid + half_range, 1.0)
end

local function compute_grid()
    grid = {}
    local spacing = (range_high - range_low) / config.n_levels
    for i = 0, config.n_levels do
        local price = round_price(range_low + i * spacing, 0.01)
        grid[i] = {
            price    = price,
            buy_oid  = nil,
            sell_oid = nil,
        }
    end
    bot.log("info", string.format("grid: computed %d levels, $%.0f–$%.0f, spacing=$%.2f",
        config.n_levels + 1, range_low, range_high, spacing))
end

local function place_buy(level)
    if not config.enabled or breakout_mode then return end
    local g = grid[level]
    if not g or g.buy_oid then return end

    local oid = bot.place_limit(config.coin, "buy", g.price, config.order_size,
                                { tif = "alo" })
    if oid then
        g.buy_oid = oid
        bot.save_state("buy_" .. level, tostring(oid))
    end
end

local function place_sell(level)
    if not config.enabled or breakout_mode then return end
    local g = grid[level]
    if not g or g.sell_oid then return end

    local oid = bot.place_limit(config.coin, "sell", g.price, config.order_size,
                                { tif = "alo" })
    if oid then
        g.sell_oid = oid
        bot.save_state("sell_" .. level, tostring(oid))
    end
end

local function find_level_by_oid(oid)
    for i = 0, config.n_levels do
        if grid[i] then
            if grid[i].buy_oid == oid then return i, "buy" end
            if grid[i].sell_oid == oid then return i, "sell" end
        end
    end
    return nil, nil
end

local function cancel_all_grid()
    bot.cancel_all(config.coin)
    for i = 0, config.n_levels do
        if grid[i] then
            grid[i].buy_oid = nil
            grid[i].sell_oid = nil
        end
    end
end

-- ── Grid setup ─────────────────────────────────────────────────────────────

local function setup_grid()
    local mid = bot.get_mid_price(config.coin)
    if not mid then
        bot.log("warn", "grid: cannot get mid price, skipping setup")
        return
    end

    cancel_all_grid()

    bot.log("info", string.format("grid: setting up around mid=$%.2f", mid))

    for i = 0, config.n_levels do
        local g = grid[i]
        if g then
            if g.price < mid - 1.0 then
                place_buy(i)
            elseif g.price > mid + 1.0 then
                place_sell(i)
            end
        end
    end
end

local function recenter_grid(mid)
    local now = bot.time()
    if now - last_recenter < config.recenter_cd then return end
    last_recenter = now

    bot.log("info", string.format("grid: recentering around $%.2f", mid))

    local new_low, new_high = compute_atr_range(mid)
    range_low = new_low
    range_high = new_high

    bot.save_state("range_low", tostring(range_low))
    bot.save_state("range_high", tostring(range_high))

    compute_grid()
    setup_grid()
end

-- ── Breakout logic ─────────────────────────────────────────────────────────

local function enter_breakout(direction, mid)
    if breakout_mode then return end
    if not config.breakout_enabled then return end

    cancel_all_grid()

    breakout_mode = true
    breakout_side = direction
    breakout_entry = mid
    breakout_peak = mid

    local side = direction == "long" and "buy" or "sell"

    bot.log("warn", string.format(
        "grid: BREAKOUT %s @ $%.2f — switching to momentum mode",
        string.upper(direction), mid))

    -- IOC entry for immediate fill
    breakout_oid = bot.place_limit(config.coin, side, mid, config.breakout_size,
                                    { tif = "ioc" })
end

local function place_breakout_sl_tp(fill_price)
    local sl_price, tp_price

    if breakout_side == "long" then
        sl_price = fill_price * (1 - config.breakout_sl_pct / 100)
        tp_price = fill_price * (1 + config.breakout_tp_pct / 100)

        breakout_sl_oid = bot.place_trigger(config.coin, "sell", sl_price,
                                             config.breakout_size, sl_price, "sl")
        breakout_tp_oid = bot.place_trigger(config.coin, "sell", tp_price,
                                             config.breakout_size, tp_price, "tp")
    else
        sl_price = fill_price * (1 + config.breakout_sl_pct / 100)
        tp_price = fill_price * (1 - config.breakout_tp_pct / 100)

        breakout_sl_oid = bot.place_trigger(config.coin, "buy", sl_price,
                                             config.breakout_size, sl_price, "sl")
        breakout_tp_oid = bot.place_trigger(config.coin, "buy", tp_price,
                                             config.breakout_size, tp_price, "tp")
    end

    bot.log("info", string.format("grid: breakout SL=$%.2f TP=$%.2f", sl_price, tp_price))
end

local function update_trailing_stop(mid)
    if not breakout_mode then return end

    local pnl_pct
    if breakout_side == "long" then
        if mid > breakout_peak then breakout_peak = mid end
        pnl_pct = ((mid - breakout_entry) / breakout_entry) * 100
    else
        if mid < breakout_peak then breakout_peak = mid end
        pnl_pct = ((breakout_entry - mid) / breakout_entry) * 100
    end

    -- Activate trailing stop once we're up enough
    if pnl_pct >= config.trail_activate then
        local new_sl

        if breakout_side == "long" then
            new_sl = breakout_peak * (1 - config.trail_offset_pct / 100)
            -- Only move SL up, never down
            local current_sl = breakout_entry * (1 - config.breakout_sl_pct / 100)
            if new_sl <= current_sl then return end
        else
            new_sl = breakout_peak * (1 + config.trail_offset_pct / 100)
            local current_sl = breakout_entry * (1 + config.breakout_sl_pct / 100)
            if new_sl >= current_sl then return end
        end

        -- Cancel old SL and place new one
        if breakout_sl_oid then
            bot.cancel(config.coin, breakout_sl_oid)
        end

        local sl_side = breakout_side == "long" and "sell" or "buy"
        breakout_sl_oid = bot.place_trigger(config.coin, sl_side, new_sl,
                                             config.breakout_size, new_sl, "sl")

        bot.log("info", string.format("grid: trailing SL moved to $%.2f (peak=$%.2f, +%.1f%%)",
            new_sl, breakout_peak, pnl_pct))
    end
end

local function exit_breakout(reason)
    bot.log("info", string.format("grid: exiting breakout mode — %s", reason))

    -- Cancel remaining SL/TP
    if breakout_sl_oid then bot.cancel(config.coin, breakout_sl_oid); breakout_sl_oid = nil end
    if breakout_tp_oid then bot.cancel(config.coin, breakout_tp_oid); breakout_tp_oid = nil end

    breakout_mode = false
    breakout_side = nil
    breakout_entry = 0
    breakout_peak = 0
    breakout_oid = nil

    -- Recenter grid around new price
    local mid = bot.get_mid_price(config.coin)
    if mid then
        recenter_grid(mid)
    end
end

-- ── 24h safety check ───────────────────────────────────────────────────────

local function check_24h_safety(mid)
    if eth_price_24h <= 0 then
        eth_price_24h = mid
        return true
    end

    local change_pct = ((mid - eth_price_24h) / eth_price_24h) * 100

    if change_pct < -config.max_drop_24h_pct then
        bot.log("warn", string.format(
            "grid: ETH dropped %.1f%% in 24h ($%.0f → $%.0f) — EMERGENCY CLOSE",
            change_pct, eth_price_24h, mid))

        -- Close everything
        if breakout_mode then
            exit_breakout("24h drop safety")
        end
        cancel_all_grid()

        -- Close any remaining position
        local pos = bot.get_position(config.coin)
        if pos and pos.size ~= 0 then
            local close_side = pos.size > 0 and "sell" or "buy"
            bot.place_limit(config.coin, close_side, mid, math.abs(pos.size),
                           { tif = "ioc", reduce_only = true })
        end

        config.enabled = false
        bot.save_state("enabled", "false")
        return false
    end

    return true
end

-- ── Integrity check ────────────────────────────────────────────────────────

local function check_integrity()
    local mid = bot.get_mid_price(config.coin)
    if not mid then return end

    -- Check if price drifted outside grid range
    local margin = (range_high - range_low) * 0.15  -- 15% buffer zone

    if mid < range_low + margin or mid > range_high - margin then
        -- Price near edge: recenter instead of waiting for breakout
        if not breakout_mode then
            recenter_grid(mid)
            return
        end
    end

    if breakout_mode then return end  -- don't check grid in breakout mode

    -- Check if price fully broke out
    if mid < range_low * 0.98 then
        enter_breakout("short", mid)
        return
    elseif mid > range_high * 1.02 then
        enter_breakout("long", mid)
        return
    end

    -- Normal integrity: repair missing orders
    local open = bot.get_open_orders(config.coin)
    local oid_set = {}
    for _, o in ipairs(open) do
        oid_set[o.oid] = true
    end

    local missing = 0
    for i = 0, config.n_levels do
        local g = grid[i]
        if g then
            if g.price < mid - 1.0 then
                if g.buy_oid and not oid_set[g.buy_oid] then
                    g.buy_oid = nil
                end
                if not g.buy_oid then
                    place_buy(i)
                    missing = missing + 1
                end
            elseif g.price > mid + 1.0 then
                if g.sell_oid and not oid_set[g.sell_oid] then
                    g.sell_oid = nil
                end
                if not g.sell_oid then
                    place_sell(i)
                    missing = missing + 1
                end
            end
        end
    end

    if missing > 0 then
        bot.log("info", string.format("grid: integrity repaired %d orders", missing))
    end
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    bot.log("info", string.format(
        "grid_eth v2: %d levels, size=%.4f ETH, lev=%dx, breakout=%s",
        config.n_levels, config.order_size, config.leverage,
        config.breakout_enabled and "ON" or "OFF"))

    -- Restore saved state
    local saved_enabled = bot.load_state("enabled")
    if saved_enabled == "false" then config.enabled = false end
    local saved_size = bot.load_state("order_size")
    if saved_size then config.order_size = tonumber(saved_size) end
    local saved_levels = bot.load_state("n_levels")
    if saved_levels then config.n_levels = tonumber(saved_levels) end
    local saved_low = bot.load_state("range_low")
    local saved_high = bot.load_state("range_high")

    -- Compute range
    local mid = bot.get_mid_price(config.coin)
    if mid then
        if saved_low and saved_high then
            range_low = tonumber(saved_low)
            range_high = tonumber(saved_high)
            -- Check if saved range is still relevant
            if mid < range_low * 0.95 or mid > range_high * 1.05 then
                bot.log("info", "grid: saved range stale, recomputing from ATR")
                range_low, range_high = compute_atr_range(mid)
            end
        else
            range_low, range_high = compute_atr_range(mid)
        end

        eth_price_24h = mid
        compute_grid()

        if config.enabled then
            setup_grid()
        else
            bot.log("warn", "grid_eth: starting DISABLED")
        end
    end

    initialized = true
    last_check = bot.time()
    last_recenter = bot.time()
end

function on_tick(coin, mid_price)
    if coin ~= config.coin then return end
    if not config.enabled and not breakout_mode then return end

    -- 24h safety
    if not check_24h_safety(mid_price) then return end

    -- Update trailing stop in breakout mode
    if breakout_mode then
        update_trailing_stop(mid_price)
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    -- Breakout entry fill?
    if fill.oid == breakout_oid and breakout_mode then
        breakout_entry = fill.price
        breakout_peak = fill.price
        breakout_oid = nil
        place_breakout_sl_tp(fill.price)
        bot.log("info", string.format("grid: breakout FILLED %s @ $%.2f",
            breakout_side, fill.price))
        return
    end

    -- Breakout SL/TP fill?
    if breakout_mode then
        if fill.oid == breakout_sl_oid then
            bot.log("info", string.format("grid: breakout SL hit — pnl=%.4f", fill.closed_pnl))
            exit_breakout("stop loss")
            return
        end
        if fill.oid == breakout_tp_oid then
            bot.log("info", string.format("grid: breakout TP hit — pnl=%.4f", fill.closed_pnl))
            exit_breakout("take profit")
            return
        end
    end

    -- Normal grid fill
    if not config.enabled then return end

    local level, side = find_level_by_oid(fill.oid)
    if not level then return end

    bot.log("info", string.format("grid: FILL level=%d %s @ $%.2f, pnl=%.4f",
        level, side, fill.price, fill.closed_pnl))

    if side == "buy" then
        grid[level].buy_oid = nil
        bot.save_state("buy_" .. level, "")
        local sell_level = level + 1
        if sell_level <= config.n_levels and grid[sell_level] then
            place_sell(sell_level)
        end
    elseif side == "sell" then
        grid[level].sell_oid = nil
        bot.save_state("sell_" .. level, "")
        local buy_level = level - 1
        if buy_level >= 0 and grid[buy_level] then
            place_buy(buy_level)
        end
    end
end

function on_timer()
    if not initialized then return end

    local now = bot.time()
    if now - last_check >= config.check_sec then
        last_check = now
        if config.enabled or breakout_mode then
            check_integrity()
        end
    end

    -- Update 24h reference every hour
    if #price_history > 0 and now % 3600 < config.check_sec then
        eth_price_24h = price_history[1]  -- oldest entry (~24h ago)
    end

    -- Store current price for 24h tracking
    local mid = bot.get_mid_price(config.coin)
    if mid then
        table.insert(price_history, mid)
        -- Keep ~24h of 1-min samples (1440)
        while #price_history > 1440 do
            table.remove(price_history, 1)
        end
    end
end

function on_book(book)
    -- Reserved for future L2-aware order placement
end

function on_advisory(json_str)
    bot.log("info", "grid_eth: advisory received: " .. json_str)

    local new_size = json_str:match('"size"%s*:%s*([%d%.]+)')
    local new_levels = json_str:match('"n_levels"%s*:%s*(%d+)')
    local pause = json_str:match('"pause"%s*:%s*(true)')
    local resume = json_str:match('"pause"%s*:%s*(false)')

    local need_rebuild = false

    if new_size then
        local v = tonumber(new_size)
        if v and v > 0 and v <= 0.05 then  -- safety cap 0.05 ETH
            config.order_size = v
            bot.save_state("order_size", tostring(v))
            bot.log("info", string.format("grid: advisory set size=%.4f", v))
            need_rebuild = true
        end
    end

    if new_levels then
        local v = tonumber(new_levels)
        if v and v >= 5 and v <= 30 then
            config.n_levels = v
            bot.save_state("n_levels", tostring(v))
            bot.log("info", string.format("grid: advisory set levels=%d", v))
            need_rebuild = true
        end
    end

    if pause then
        config.enabled = false
        bot.save_state("enabled", "false")
        cancel_all_grid()
        if breakout_mode then exit_breakout("advisory pause") end
        bot.log("warn", "grid: PAUSED by advisory")
        return
    end

    if resume and not config.enabled then
        config.enabled = true
        bot.save_state("enabled", "true")
        bot.log("info", "grid: RESUMED by advisory")
        need_rebuild = true
    end

    if need_rebuild then
        local mid = bot.get_mid_price(config.coin)
        if mid then recenter_grid(mid) end
    end
end

function on_shutdown()
    bot.log("info", "grid_eth: shutting down")

    if breakout_mode then
        -- Keep breakout SL/TP on exchange — don't close
        bot.log("info", "grid: breakout position open, SL/TP remain on exchange")
    end

    cancel_all_grid()

    bot.save_state("range_low", tostring(range_low))
    bot.save_state("range_high", tostring(range_high))
    bot.save_state("order_size", tostring(config.order_size))
    bot.save_state("n_levels", tostring(config.n_levels))
    bot.save_state("enabled", tostring(config.enabled))
end
