#ifndef TB_STRATEGY_API_H
#define TB_STRATEGY_API_H

#include "strategy/lua_engine.h"
#include <lua.h>

/*
 * Register all bot.* functions into the given lua_State.
 * The ctx pointer is stored in the Lua registry for access from C functions.
 *
 * API exposed to Lua:
 *   bot.place_limit(coin, side, price, size, opts)  → oid or nil, err
 *   bot.place_trigger(coin, side, price, size, trigger_px, tpsl) → oid or nil, err
 *   bot.cancel(coin, oid) → ok, err
 *   bot.cancel_all(coin) → ok, err
 *   bot.get_position(coin) → {size, entry_px, pnl, liq_px} or nil
 *   bot.get_mid_price(coin) → price or nil
 *   bot.get_open_orders(coin) → {{oid, side, price, size}, ...}
 *   bot.get_candles(coin, interval, count) → {{t, o, h, l, c, v}, ...}
 *   bot.get_account_value() → number
 *   bot.get_daily_pnl() → number
 *   bot.save_state(key, value) → ok
 *   bot.load_state(key) → value or nil
 *   bot.log(level, msg) → nil
 *   bot.time() → epoch seconds (float)
 *   bot.sleep_ms(ms) → nil  (cooperative, just sets timer)
 */
void tb_strategy_api_register(lua_State *L, tb_lua_ctx_t *ctx);

/* Update the context pointer (e.g. when strategy name changes) */
void tb_strategy_api_set_context(lua_State *L, tb_lua_ctx_t *ctx);

#endif /* TB_STRATEGY_API_H */
