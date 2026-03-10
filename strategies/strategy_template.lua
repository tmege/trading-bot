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

-- Called once after loading
function on_init()
    bot.log("info", "strategy template initialized")
end

-- Called on mid price update
function on_tick(coin, mid_price)
    -- Example: log every tick
    -- bot.log("debug", string.format("%s mid=%.2f", coin, mid_price))
end

-- Called when one of our orders is filled
function on_fill(fill)
    bot.log("info", string.format("fill: %s %s %.4f @ %.2f pnl=%.4f",
        fill.side, fill.coin, fill.size, fill.price, fill.closed_pnl))
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
