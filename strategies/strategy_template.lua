--[[
  Strategy Template

  Copy this file and rename it to create a new strategy.
  Implement the callbacks you need; unused ones can be omitted.

  Available API (bot.*):
    bot.place_limit(coin, side, price, size [, opts])  -> oid or nil, err
    bot.place_trigger(coin, side, price, size, trigger_px, tpsl) -> oid or nil, err
    bot.cancel(coin, oid)            -> ok, err
    bot.cancel_all(coin)             -> ok
    bot.get_position(coin)           -> {size, entry_px, ...} or nil
    bot.get_mid_price(coin)          -> number or nil
    bot.get_open_orders(coin)        -> {{oid, side, price, size}, ...}
    bot.get_candles(coin, interval, count) -> {{t,o,h,l,c,v}, ...}
    bot.get_account_value()          -> number
    bot.get_daily_pnl()              -> number
    bot.save_state(key, value)       -> ok
    bot.load_state(key)              -> value or nil
    bot.log(level, msg)              -> nil    (level: "debug","info","warn","error")
    bot.time()                       -> epoch seconds (float)

  Opts table for place_limit:
    { reduce_only = false, tif = "gtc"|"ioc"|"alo", cloid = "..." }
]]

-- ── Crash protection ──────────────────────────────────────────────────────
-- Copy these variables and the velocity_check function into your strategy.
-- local price_history     = {}      -- {price, time} ring buffer
-- local PRICE_HIST_SIZE   = 30      -- track last 30 price samples
-- local VELOCITY_THRESH   = 10.0    -- skip entry if >10% move in window
-- local losing_streak     = 0
-- local MAX_LOSING_STREAK = 3       -- pause 5 min after 3 consecutive losses
-- local streak_pause_until = 0
--
-- local function velocity_check(mid_price, now)
--     table.insert(price_history, {p = mid_price, t = now})
--     if #price_history > PRICE_HIST_SIZE then
--         table.remove(price_history, 1)
--     end
--     if #price_history < 5 then return true end
--     local min_p, max_p = mid_price, mid_price
--     for _, v in ipairs(price_history) do
--         if v.p < min_p then min_p = v.p end
--         if v.p > max_p then max_p = v.p end
--     end
--     local move_pct = ((max_p - min_p) / min_p) * 100
--     if move_pct >= VELOCITY_THRESH then
--         bot.log("warn", string.format("VELOCITY GUARD — %.1f%% move detected, skipping entry", move_pct))
--         return false
--     end
--     return true
-- end

-- Called once after loading
function on_init()
    bot.log("info", "strategy template initialized")
end

-- Called on mid price update
function on_tick(coin, mid_price)
    -- Example: log every tick
    -- bot.log("debug", string.format("%s mid=%.2f", coin, mid_price))

    -- Crash protection guards (add after cooldown check, before entry logic):
    -- if losing_streak >= MAX_LOSING_STREAK and now < streak_pause_until then
    --     return
    -- elseif losing_streak >= MAX_LOSING_STREAK then
    --     losing_streak = 0
    -- end
    -- if not velocity_check(mid_price, now) then return end
end

-- Called when one of our orders is filled
function on_fill(fill)
    bot.log("info", string.format("fill: %s %s %.4f @ %.2f pnl=%.4f",
        fill.side, fill.coin, fill.size, fill.price, fill.closed_pnl))

    -- Crash protection: losing streak tracking (add in exit fill section):
    -- if fill.closed_pnl > 0 then
    --     losing_streak = 0
    -- else
    --     losing_streak = losing_streak + 1
    --     if losing_streak >= MAX_LOSING_STREAK then
    --         streak_pause_until = bot.time() + 300
    --         bot.log("warn", string.format("%d consecutive losses — pausing 5 min", losing_streak))
    --     end
    -- end
end

-- Called periodically (every ~60s)
function on_timer()
    -- Example: check position, adjust orders
end

-- Called on L2 book update
function on_book(book)
    -- book.coin, book.bids[i].price/size, book.asks[i].price/size
end

-- Called when AI advisory sends adjustments (JSON string)
function on_advisory(json_str)
    bot.log("info", "advisory received: " .. json_str)
end

-- Called on graceful shutdown
function on_shutdown()
    bot.log("info", "strategy shutting down")
end
