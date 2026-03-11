#include "strategy/strategy_api.h"
#include "strategy/lua_engine.h"
#include "exchange/order_manager.h"
#include "exchange/position_tracker.h"
#include "exchange/hl_rest.h"
#include "risk/risk_manager.h"
#include "data/data_manager.h"
#include "strategy/indicators.h"
#include "core/logging.h"
#include "core/types.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* Registry key for our context pointer */
static const char *CTX_REGISTRY_KEY = "tb_lua_ctx";

/* ── Candle cache for get_indicators ────────────────────────────────────── */
/* Caches fetched candles by coin+interval with a 30s TTL.
 * Thread-safe because all strategies execute sequentially under engine->lock. */
#define CANDLE_CACHE_SIZE 8
#define CANDLE_CACHE_TTL_MS 30000
#define CANDLE_CACHE_MAX 300

typedef struct {
    char    coin[16];
    char    interval[8];
    double  closes[CANDLE_CACHE_MAX];
    double  highs[CANDLE_CACHE_MAX];
    double  lows[CANDLE_CACHE_MAX];
    double  opens[CANDLE_CACHE_MAX];
    double  volumes[CANDLE_CACHE_MAX];
    int64_t times[CANDLE_CACHE_MAX];
    int     count;
    int64_t fetched_at_ms;
} candle_cache_t;

static candle_cache_t g_candle_cache[CANDLE_CACHE_SIZE];

static int64_t cache_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Find a cache entry for coin+interval that is still fresh. Returns index or -1. */
static int candle_cache_find(const char *coin, const char *interval) {
    int64_t now = cache_now_ms();
    for (int i = 0; i < CANDLE_CACHE_SIZE; i++) {
        if (g_candle_cache[i].count > 0 &&
            strcmp(g_candle_cache[i].coin, coin) == 0 &&
            strcmp(g_candle_cache[i].interval, interval) == 0 &&
            (now - g_candle_cache[i].fetched_at_ms) < CANDLE_CACHE_TTL_MS) {
            return i;
        }
    }
    return -1;
}

/* Store candles in cache, evicting the oldest entry if full. */
static int candle_cache_store(const char *coin, const char *interval,
                               const tb_candle_input_t *input, int count) {
    if (count > CANDLE_CACHE_MAX) count = CANDLE_CACHE_MAX;

    /* Find existing slot or oldest entry to evict */
    int slot = -1;
    int64_t oldest_ts = INT64_MAX;
    for (int i = 0; i < CANDLE_CACHE_SIZE; i++) {
        if (g_candle_cache[i].count == 0 ||
            (strcmp(g_candle_cache[i].coin, coin) == 0 &&
             strcmp(g_candle_cache[i].interval, interval) == 0)) {
            slot = i;
            break;
        }
        if (g_candle_cache[i].fetched_at_ms < oldest_ts) {
            oldest_ts = g_candle_cache[i].fetched_at_ms;
            slot = i;
        }
    }
    if (slot < 0) slot = 0;

    candle_cache_t *c = &g_candle_cache[slot];
    snprintf(c->coin, sizeof(c->coin), "%s", coin);
    snprintf(c->interval, sizeof(c->interval), "%s", interval);
    c->count = count;
    c->fetched_at_ms = cache_now_ms();
    for (int i = 0; i < count; i++) {
        c->opens[i]   = input[i].open;
        c->highs[i]   = input[i].high;
        c->lows[i]    = input[i].low;
        c->closes[i]  = input[i].close;
        c->volumes[i] = input[i].volume;
        c->times[i]   = input[i].time_ms;
    }
    return slot;
}

/* Validate a string for safe JSON interpolation: alphanumeric + limited
 * punctuation, bounded length. Used for coin names and intervals. */
static bool is_safe_json_value(const char *s, size_t max_len) {
    if (!s || !s[0]) return false;
    size_t len = strlen(s);
    if (len > max_len) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

static bool is_valid_coin(const char *s) {
    return is_safe_json_value(s, 15);
}

/* Parse interval string ("1m","5m","15m","1h","4h","1d") to seconds */
static int interval_to_sec(const char *interval) {
    int n = atoi(interval);
    if (n <= 0) n = 1;
    const char *p = interval;
    while (*p >= '0' && *p <= '9') p++;
    switch (*p) {
        case 'm': return n * 60;
        case 'h': return n * 3600;
        case 'd': return n * 86400;
        case 'w': return n * 604800;
        default:  return 3600; /* fallback to 1h */
    }
}

static tb_lua_ctx_t *get_ctx(lua_State *L) {
    lua_pushlightuserdata(L, (void *)CTX_REGISTRY_KEY);
    lua_gettable(L, LUA_REGISTRYINDEX);
    tb_lua_ctx_t *ctx = (tb_lua_ctx_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

/* ── bot.place_limit(coin, side, price, size [, opts_table]) ────────────── */
static int api_place_limit(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->order_mgr) {
        lua_pushnil(L);
        lua_pushstring(L, "no order manager");
        return 2;
    }

    const char *coin = luaL_checkstring(L, 1);
    const char *side_str = luaL_checkstring(L, 2);
    double price = luaL_checknumber(L, 3);
    double size = luaL_checknumber(L, 4);

    /* Validate coin name */
    if (!is_valid_coin(coin)) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid coin name");
        return 2;
    }

    /* Validate price and size */
    if (!isfinite(price) || price <= 0.0) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid price (must be positive finite number)");
        return 2;
    }
    if (!isfinite(size) || size <= 0.0) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid size (must be positive finite number)");
        return 2;
    }

    tb_order_submit_t submit;
    memset(&submit, 0, sizeof(submit));
    snprintf(submit.strategy_name, sizeof(submit.strategy_name), "%s",
             ctx->strategy_name ? ctx->strategy_name : "unknown");

    tb_order_request_t *order = &submit.order;
    snprintf(order->coin, sizeof(order->coin), "%s", coin);
    order->side = (strcmp(side_str, "sell") == 0) ? TB_SIDE_SELL : TB_SIDE_BUY;
    order->price = tb_decimal_from_double(price, 6);
    order->size = tb_decimal_from_double(size, 6);
    order->type = TB_ORDER_LIMIT;
    order->tif = TB_TIF_GTC;

    /* Parse optional table */
    if (lua_istable(L, 5)) {
        lua_getfield(L, 5, "reduce_only");
        if (lua_isboolean(L, -1)) order->reduce_only = lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 5, "tif");
        if (lua_isstring(L, -1)) {
            const char *tif = lua_tostring(L, -1);
            if (strcmp(tif, "ioc") == 0) order->tif = TB_TIF_IOC;
            else if (strcmp(tif, "alo") == 0) order->tif = TB_TIF_ALO;
        }
        lua_pop(L, 1);

        lua_getfield(L, 5, "cloid");
        if (lua_isstring(L, -1)) {
            snprintf(order->cloid, sizeof(order->cloid), "%s", lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }

    uint64_t oid = 0;
    int rc = tb_order_mgr_submit(ctx->order_mgr, &submit, &oid);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "order submission failed");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)oid);
    return 1;
}

/* ── bot.place_trigger(coin, side, price, size, trigger_px, tpsl) ───────── */
static int api_place_trigger(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->order_mgr) {
        lua_pushnil(L);
        lua_pushstring(L, "no order manager");
        return 2;
    }

    const char *coin = luaL_checkstring(L, 1);
    const char *side_str = luaL_checkstring(L, 2);
    double price = luaL_checknumber(L, 3);
    double size = luaL_checknumber(L, 4);
    double trigger_px = luaL_checknumber(L, 5);
    const char *tpsl_str = luaL_optstring(L, 6, "sl");

    /* Validate coin name */
    if (!is_valid_coin(coin)) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid coin name");
        return 2;
    }

    /* Validate price, size and trigger */
    if (!isfinite(price) || price <= 0.0 ||
        !isfinite(size) || size <= 0.0 ||
        !isfinite(trigger_px) || trigger_px <= 0.0) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid price/size/trigger (must be positive finite)");
        return 2;
    }

    tb_order_submit_t submit;
    memset(&submit, 0, sizeof(submit));
    snprintf(submit.strategy_name, sizeof(submit.strategy_name), "%s",
             ctx->strategy_name ? ctx->strategy_name : "unknown");

    tb_order_request_t *order = &submit.order;
    snprintf(order->coin, sizeof(order->coin), "%s", coin);
    order->side = (strcmp(side_str, "sell") == 0) ? TB_SIDE_SELL : TB_SIDE_BUY;

    /* Slippage guard: isMarket=true still needs a limit price guardrail.
       Without slack, any slippage past triggerPx prevents the fill.
       Use 10% margin (same as Hyperliquid Python SDK). */
    double limit_px = price;
    if (order->side == TB_SIDE_SELL) {
        limit_px = trigger_px * 0.90;
    } else {
        limit_px = trigger_px * 1.10;
    }
    order->price = tb_decimal_from_double(limit_px, 6);
    order->size = tb_decimal_from_double(size, 6);
    order->type = TB_ORDER_TRIGGER;
    order->is_market = true;
    order->trigger_px = tb_decimal_from_double(trigger_px, 6);
    order->tpsl = (strcmp(tpsl_str, "tp") == 0) ? TB_TPSL_TP : TB_TPSL_SL;
    order->reduce_only = true;
    /* Use TB_GROUP_NA: positionTpsl requires SL+TP in a single batch call,
       but we send them separately — the 2nd overwrites the 1st.
       With NA, each trigger is independent and both coexist. */
    order->grouping = TB_GROUP_NA;

    uint64_t oid = 0;
    int rc = tb_order_mgr_submit(ctx->order_mgr, &submit, &oid);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "trigger order failed");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)oid);
    return 1;
}

/* ── bot.cancel(coin, oid) ──────────────────────────────────────────────── */
static int api_cancel(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->order_mgr) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no order manager");
        return 2;
    }

    const char *coin = luaL_checkstring(L, 1);
    lua_Integer oid = luaL_checkinteger(L, 2);

    int rc = tb_order_mgr_cancel_by_coin(ctx->order_mgr, coin, (uint64_t)oid);
    lua_pushboolean(L, rc == 0);
    if (rc != 0) {
        lua_pushstring(L, "cancel failed");
        return 2;
    }
    return 1;
}

/* ── bot.cancel_all(coin) ───────────────────────────────────────────────── */
static int api_cancel_all(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->order_mgr) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no order manager");
        return 2;
    }

    const char *coin = luaL_checkstring(L, 1);
    if (!is_valid_coin(coin)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid coin");
        return 2;
    }
    int rc = tb_order_mgr_cancel_all_coin(ctx->order_mgr, coin);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── bot.get_position(coin) → table or nil ──────────────────────────────── */
static int api_get_position(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->pos_tracker) {
        lua_pushnil(L);
        return 1;
    }

    const char *coin = luaL_checkstring(L, 1);
    if (!is_valid_coin(coin)) {
        lua_pushnil(L);
        return 1;
    }
    tb_position_t pos;
    if (tb_pos_tracker_get(ctx->pos_tracker, coin, &pos) != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 6);

    lua_pushnumber(L, tb_decimal_to_double(pos.size));
    lua_setfield(L, -2, "size");

    lua_pushnumber(L, tb_decimal_to_double(pos.entry_px));
    lua_setfield(L, -2, "entry_px");

    lua_pushnumber(L, tb_decimal_to_double(pos.unrealized_pnl));
    lua_setfield(L, -2, "unrealized_pnl");

    lua_pushnumber(L, tb_decimal_to_double(pos.realized_pnl));
    lua_setfield(L, -2, "realized_pnl");

    lua_pushnumber(L, tb_decimal_to_double(pos.liquidation_px));
    lua_setfield(L, -2, "liquidation_px");

    lua_pushinteger(L, pos.leverage);
    lua_setfield(L, -2, "leverage");

    return 1;
}

/* ── bot.get_mid_price(coin) → number or nil ────────────────────────────── */
static int api_get_mid_price(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->rest) {
        lua_pushnil(L);
        return 1;
    }

    const char *coin = luaL_checkstring(L, 1);

    tb_mid_t mids[512];
    int count = 0;
    if (hl_rest_get_all_mids(ctx->rest, mids, &count) != 0) {
        lua_pushnil(L);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        if (strcmp(mids[i].coin, coin) == 0) {
            lua_pushnumber(L, tb_decimal_to_double(mids[i].mid));
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

/* ── bot.get_open_orders(coin) → array of tables ────────────────────────── */
static int api_get_open_orders(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->order_mgr) {
        lua_newtable(L);
        return 1;
    }

    const char *coin = luaL_checkstring(L, 1);
    if (!is_valid_coin(coin)) {
        lua_newtable(L);
        return 1;
    }
    tb_order_t orders[128];
    int count = 128;
    tb_order_mgr_get_open_orders(ctx->order_mgr, coin, orders, &count);

    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        lua_createtable(L, 0, 4);

        lua_pushinteger(L, (lua_Integer)orders[i].oid);
        lua_setfield(L, -2, "oid");

        lua_pushstring(L, orders[i].side == TB_SIDE_BUY ? "buy" : "sell");
        lua_setfield(L, -2, "side");

        lua_pushnumber(L, tb_decimal_to_double(orders[i].limit_px));
        lua_setfield(L, -2, "price");

        lua_pushnumber(L, tb_decimal_to_double(orders[i].sz));
        lua_setfield(L, -2, "size");

        lua_rawseti(L, -2, i + 1);
    }

    return 1;
}

/* ── bot.get_candles(coin, interval, count) → array ─────────────────────── */
static int api_get_candles(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->rest) {
        lua_newtable(L);
        return 1;
    }

    const char *coin = luaL_checkstring(L, 1);
    if (!is_valid_coin(coin)) {
        lua_newtable(L);
        return 1;
    }
    const char *interval = luaL_optstring(L, 2, "1h");
    if (!is_safe_json_value(interval, 4)) interval = "1h";
    int req_count = (int)luaL_optinteger(L, 3, 100);
    if (req_count > 500) req_count = 500;

    /* Fetch recent candles */
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    int64_t start_ms = now_ms - (int64_t)req_count * interval_to_sec(interval) * 1000;

    tb_candle_t *candles = malloc(sizeof(tb_candle_t) * (size_t)req_count);
    if (!candles) { lua_newtable(L); return 1; }
    int count = 0;
    if (hl_rest_get_candles(ctx->rest, coin, interval, start_ms, now_ms,
                            candles, &count, req_count) != 0) {
        free(candles);
        lua_newtable(L);
        return 1;
    }

    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        lua_createtable(L, 0, 6);

        lua_pushinteger(L, candles[i].time_open);
        lua_setfield(L, -2, "t");

        lua_pushnumber(L, tb_decimal_to_double(candles[i].open));
        lua_setfield(L, -2, "o");

        lua_pushnumber(L, tb_decimal_to_double(candles[i].high));
        lua_setfield(L, -2, "h");

        lua_pushnumber(L, tb_decimal_to_double(candles[i].low));
        lua_setfield(L, -2, "l");

        lua_pushnumber(L, tb_decimal_to_double(candles[i].close));
        lua_setfield(L, -2, "c");

        lua_pushnumber(L, tb_decimal_to_double(candles[i].volume));
        lua_setfield(L, -2, "v");

        lua_rawseti(L, -2, i + 1);
    }

    free(candles);
    return 1;
}

/* ── bot.get_account_value() → number ───────────────────────────────────── */
static int api_get_account_value(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->pos_tracker) {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, tb_pos_tracker_account_value(ctx->pos_tracker));
    return 1;
}

/* ── bot.get_daily_pnl() → number ──────────────────────────────────────── */
static int api_get_daily_pnl(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->pos_tracker) {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, tb_pos_tracker_daily_pnl(ctx->pos_tracker));
    return 1;
}

/* ── bot.save_state(key, value) ─────────────────────────────────────────── */
static int api_save_state(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    const char *key = luaL_checkstring(L, 1);

    /* Serialize Lua value to a simple string representation */
    const char *val = NULL;
    char num_buf[64];
    if (lua_isnumber(L, 2)) {
        snprintf(num_buf, sizeof(num_buf), "%.17g", lua_tonumber(L, 2));
        val = num_buf;
    } else if (lua_isstring(L, 2)) {
        val = lua_tostring(L, 2);
    } else if (lua_isboolean(L, 2)) {
        val = lua_toboolean(L, 2) ? "true" : "false";
    } else {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Store in Lua registry under a strategy-specific table */
    lua_pushlightuserdata(L, (void *)ctx); /* use ctx address as table key */
    lua_gettable(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushlightuserdata(L, (void *)ctx);
        lua_newtable(L);
        lua_settable(L, LUA_REGISTRYINDEX);
        lua_pushlightuserdata(L, (void *)ctx);
        lua_gettable(L, LUA_REGISTRYINDEX);
    }
    lua_pushstring(L, val);
    lua_setfield(L, -2, key);
    lua_pop(L, 1);

    lua_pushboolean(L, 1);
    return 1;
}

/* ── bot.load_state(key) → value or nil ─────────────────────────────────── */
static int api_load_state(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    const char *key = luaL_checkstring(L, 1);

    lua_pushlightuserdata(L, (void *)ctx);
    lua_gettable(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
        lua_pushnil(L);
        return 1;
    }
    lua_getfield(L, -1, key);
    return 1;
}

/* ── bot.log(level, msg) ────────────────────────────────────────────────── */
static int api_log(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    const char *level = luaL_checkstring(L, 1);
    const char *msg = luaL_checkstring(L, 2);
    const char *strat = ctx ? ctx->strategy_name : "lua";

    if (strcmp(level, "debug") == 0) {
        tb_log_debug("[%s] %s", strat, msg);
    } else if (strcmp(level, "warn") == 0) {
        tb_log_warn("[%s] %s", strat, msg);
    } else if (strcmp(level, "error") == 0) {
        tb_log_error("[%s] %s", strat, msg);
    } else {
        tb_log_info("[%s] %s", strat, msg);
    }

    return 0;
}

/* ── bot.time() → epoch seconds as float ────────────────────────────────── */
static int api_time(lua_State *L) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    lua_pushnumber(L, (double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
    return 1;
}

/* ── bot.get_macro() → table ─────────────────────────────────────────────── */
static int api_get_macro(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->data_mgr) {
        lua_newtable(L);
        return 1;
    }

    tb_macro_data_t m = tb_data_mgr_get_macro(ctx->data_mgr);
    lua_createtable(L, 0, 8);

    lua_pushnumber(L, m.sp500);       lua_setfield(L, -2, "sp500");
    lua_pushnumber(L, m.gold);        lua_setfield(L, -2, "gold");
    lua_pushnumber(L, m.dxy);         lua_setfield(L, -2, "dxy");
    lua_pushnumber(L, m.btc_price);   lua_setfield(L, -2, "btc_price");
    lua_pushnumber(L, m.btc_dominance); lua_setfield(L, -2, "btc_dominance");
    lua_pushnumber(L, m.eth_btc);     lua_setfield(L, -2, "eth_btc");
    lua_pushnumber(L, m.total2_mcap); lua_setfield(L, -2, "total2_mcap");
    lua_pushboolean(L, m.valid);      lua_setfield(L, -2, "valid");

    return 1;
}

/* ── bot.get_sentiment() → table ─────────────────────────────────────────── */
static int api_get_sentiment(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->data_mgr) {
        lua_newtable(L);
        return 1;
    }

    tb_sentiment_data_t s = tb_data_mgr_get_sentiment(ctx->data_mgr);
    lua_createtable(L, 0, 6);

    lua_pushnumber(L, s.overall_score); lua_setfield(L, -2, "score");
    lua_pushnumber(L, s.bullish_pct);   lua_setfield(L, -2, "bullish_pct");
    lua_pushnumber(L, s.bearish_pct);   lua_setfield(L, -2, "bearish_pct");
    lua_pushinteger(L, s.total_tweets); lua_setfield(L, -2, "total_tweets");
    lua_pushboolean(L, s.valid);        lua_setfield(L, -2, "valid");

    /* Per-account scores */
    lua_createtable(L, s.n_accounts, 0);
    for (int i = 0; i < s.n_accounts; i++) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, s.accounts[i].account);
        lua_setfield(L, -2, "account");
        lua_pushnumber(L, s.accounts[i].score);
        lua_setfield(L, -2, "score");
        lua_pushinteger(L, s.accounts[i].tweet_count);
        lua_setfield(L, -2, "tweets");
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "accounts");

    return 1;
}

/* ── bot.get_fear_greed() → table ────────────────────────────────────────── */
static int api_get_fear_greed(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->data_mgr) {
        lua_newtable(L);
        return 1;
    }

    tb_fear_greed_t fg = tb_data_mgr_get_fear_greed(ctx->data_mgr);
    lua_createtable(L, 0, 3);

    lua_pushinteger(L, fg.value);     lua_setfield(L, -2, "value");
    lua_pushstring(L, fg.label);      lua_setfield(L, -2, "label");
    lua_pushboolean(L, fg.valid);     lua_setfield(L, -2, "valid");

    return 1;
}

/* ── bot.get_indicators(coin [, interval, count]) → table ────────────────── */
static int api_get_indicators(lua_State *L) {
    tb_lua_ctx_t *ctx = get_ctx(L);
    if (!ctx || !ctx->rest) {
        lua_newtable(L);
        return 1;
    }

    const char *coin = luaL_checkstring(L, 1);
    if (!is_valid_coin(coin)) {
        lua_newtable(L);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "valid");
        return 1;
    }
    const char *interval = luaL_optstring(L, 2, "1h");
    if (!is_safe_json_value(interval, 4)) interval = "1h";
    int req_count = (int)luaL_optinteger(L, 3, 200);
    if (req_count > 500) req_count = 500;
    if (req_count < 20) req_count = 20;
    /* Optional 4th arg: current mid_price for live indicator updates.
     * Without this, RSI/BB only update when a new candle closes. */
    double live_price = luaL_optnumber(L, 4, 0);

    /* Check candle cache first (30s TTL). On hit, reuse cached candles
     * and only inject the fresh live_price. Saves ~1000 weight/min. */
    int cache_idx = candle_cache_find(coin, interval);
    tb_candle_input_t *input = NULL;
    int count = 0;

    if (cache_idx >= 0) {
        /* Cache hit — copy cached data into input array */
        candle_cache_t *cc = &g_candle_cache[cache_idx];
        count = cc->count;
        input = malloc(sizeof(tb_candle_input_t) * (size_t)count);
        if (!input) { lua_newtable(L); return 1; }
        for (int i = 0; i < count; i++) {
            input[i].open    = cc->opens[i];
            input[i].high    = cc->highs[i];
            input[i].low     = cc->lows[i];
            input[i].close   = cc->closes[i];
            input[i].volume  = cc->volumes[i];
            input[i].time_ms = cc->times[i];
        }
    } else {
        /* Cache miss — fetch from REST API */
        int64_t now_val = (int64_t)time(NULL) * 1000;
        int64_t start_ms = now_val - (int64_t)req_count * interval_to_sec(interval) * 1000;

        tb_candle_t *candles = malloc(sizeof(tb_candle_t) * (size_t)req_count);
        if (!candles) { lua_newtable(L); return 1; }
        if (hl_rest_get_candles(ctx->rest, coin, interval, start_ms, now_val,
                                candles, &count, req_count) != 0 || count < 2) {
            free(candles);
            lua_newtable(L);
            lua_pushboolean(L, 0);
            lua_setfield(L, -2, "valid");
            return 1;
        }

        /* Convert to indicator input format */
        input = malloc(sizeof(tb_candle_input_t) * (size_t)count);
        if (!input) {
            free(candles);
            lua_newtable(L);
            return 1;
        }
        for (int i = 0; i < count; i++) {
            input[i].open    = tb_decimal_to_double(candles[i].open);
            input[i].high    = tb_decimal_to_double(candles[i].high);
            input[i].low     = tb_decimal_to_double(candles[i].low);
            input[i].close   = tb_decimal_to_double(candles[i].close);
            input[i].volume  = tb_decimal_to_double(candles[i].volume);
            input[i].time_ms = candles[i].time_open;
        }
        free(candles);

        /* Store in cache for next caller */
        candle_cache_store(coin, interval, input, count);
    }

    /* Inject live price into last candle so RSI/BB reflect current price,
     * not just the last closed candle. This is critical for intra-candle
     * signal detection (e.g. 15m candles checked every 15s). */
    if (live_price > 0 && count > 0) {
        input[count - 1].close = live_price;
        if (live_price > input[count - 1].high)
            input[count - 1].high = live_price;
        if (live_price < input[count - 1].low)
            input[count - 1].low = live_price;
    }

    tb_indicators_snapshot_t snap = tb_indicators_compute(input, count);
    free(input);

    /* Build Lua table */
    lua_createtable(L, 0, 50);

    lua_pushboolean(L, snap.valid);        lua_setfield(L, -2, "valid");

    /* Moving averages */
    lua_pushnumber(L, snap.sma_20);        lua_setfield(L, -2, "sma_20");
    lua_pushnumber(L, snap.sma_50);        lua_setfield(L, -2, "sma_50");
    lua_pushnumber(L, snap.sma_200);       lua_setfield(L, -2, "sma_200");
    lua_pushnumber(L, snap.ema_12);        lua_setfield(L, -2, "ema_12");
    lua_pushnumber(L, snap.ema_26);        lua_setfield(L, -2, "ema_26");

    /* RSI */
    lua_pushnumber(L, snap.rsi_14);        lua_setfield(L, -2, "rsi");

    /* MACD */
    lua_pushnumber(L, snap.macd_line);     lua_setfield(L, -2, "macd");
    lua_pushnumber(L, snap.macd_signal);   lua_setfield(L, -2, "macd_signal");
    lua_pushnumber(L, snap.macd_histogram);lua_setfield(L, -2, "macd_histogram");

    /* Bollinger */
    lua_pushnumber(L, snap.bb_upper);      lua_setfield(L, -2, "bb_upper");
    lua_pushnumber(L, snap.bb_middle);     lua_setfield(L, -2, "bb_middle");
    lua_pushnumber(L, snap.bb_lower);      lua_setfield(L, -2, "bb_lower");
    lua_pushnumber(L, snap.bb_width);      lua_setfield(L, -2, "bb_width");

    /* ATR */
    lua_pushnumber(L, snap.atr_14);        lua_setfield(L, -2, "atr");

    /* VWAP */
    lua_pushnumber(L, snap.vwap);          lua_setfield(L, -2, "vwap");

    /* ADX */
    lua_pushnumber(L, snap.adx_14);        lua_setfield(L, -2, "adx");
    lua_pushnumber(L, snap.plus_di);       lua_setfield(L, -2, "plus_di");
    lua_pushnumber(L, snap.minus_di);      lua_setfield(L, -2, "minus_di");

    /* Keltner Channels */
    lua_pushnumber(L, snap.kc_upper);      lua_setfield(L, -2, "kc_upper");
    lua_pushnumber(L, snap.kc_middle);     lua_setfield(L, -2, "kc_middle");
    lua_pushnumber(L, snap.kc_lower);      lua_setfield(L, -2, "kc_lower");

    /* Donchian Channels */
    lua_pushnumber(L, snap.dc_upper);      lua_setfield(L, -2, "dc_upper");
    lua_pushnumber(L, snap.dc_lower);      lua_setfield(L, -2, "dc_lower");
    lua_pushnumber(L, snap.dc_middle);     lua_setfield(L, -2, "dc_middle");

    /* Stochastic RSI */
    lua_pushnumber(L, snap.stoch_rsi_k);   lua_setfield(L, -2, "stoch_rsi_k");
    lua_pushnumber(L, snap.stoch_rsi_d);   lua_setfield(L, -2, "stoch_rsi_d");

    /* CCI */
    lua_pushnumber(L, snap.cci_20);        lua_setfield(L, -2, "cci");

    /* Williams %R */
    lua_pushnumber(L, snap.williams_r);    lua_setfield(L, -2, "williams_r");

    /* OBV */
    lua_pushnumber(L, snap.obv);           lua_setfield(L, -2, "obv");
    lua_pushnumber(L, snap.obv_sma);       lua_setfield(L, -2, "obv_sma");

    /* Ichimoku */
    lua_pushnumber(L, snap.ichi_tenkan);   lua_setfield(L, -2, "ichi_tenkan");
    lua_pushnumber(L, snap.ichi_kijun);    lua_setfield(L, -2, "ichi_kijun");
    lua_pushnumber(L, snap.ichi_senkou_a); lua_setfield(L, -2, "ichi_senkou_a");
    lua_pushnumber(L, snap.ichi_senkou_b); lua_setfield(L, -2, "ichi_senkou_b");
    lua_pushnumber(L, snap.ichi_chikou);   lua_setfield(L, -2, "ichi_chikou");

    /* Signals */
    lua_pushboolean(L, snap.above_sma_200);     lua_setfield(L, -2, "above_sma200");
    lua_pushboolean(L, snap.golden_cross);       lua_setfield(L, -2, "golden_cross");
    lua_pushboolean(L, snap.rsi_oversold);       lua_setfield(L, -2, "rsi_oversold");
    lua_pushboolean(L, snap.rsi_overbought);     lua_setfield(L, -2, "rsi_overbought");
    lua_pushboolean(L, snap.bb_squeeze);         lua_setfield(L, -2, "bb_squeeze");
    lua_pushboolean(L, snap.macd_bullish_cross); lua_setfield(L, -2, "macd_bullish");
    lua_pushboolean(L, snap.adx_trending);       lua_setfield(L, -2, "adx_trending");
    lua_pushboolean(L, snap.kc_squeeze);         lua_setfield(L, -2, "kc_squeeze");
    lua_pushboolean(L, snap.ichi_bullish);       lua_setfield(L, -2, "ichi_bullish");

    return 1;
}

/* ── Registration ───────────────────────────────────────────────────────── */

static const luaL_Reg bot_funcs[] = {
    {"place_limit",     api_place_limit},
    {"place_trigger",   api_place_trigger},
    {"cancel",          api_cancel},
    {"cancel_all",      api_cancel_all},
    {"get_position",    api_get_position},
    {"get_mid_price",   api_get_mid_price},
    {"get_open_orders", api_get_open_orders},
    {"get_candles",     api_get_candles},
    {"get_account_value", api_get_account_value},
    {"get_daily_pnl",   api_get_daily_pnl},
    {"save_state",      api_save_state},
    {"load_state",      api_load_state},
    {"log",             api_log},
    {"time",            api_time},
    {"get_macro",       api_get_macro},
    {"get_sentiment",   api_get_sentiment},
    {"get_fear_greed",  api_get_fear_greed},
    {"get_indicators",  api_get_indicators},
    {NULL, NULL}
};

void tb_strategy_api_register(lua_State *L, tb_lua_ctx_t *ctx) {
    /* Store context in registry */
    lua_pushlightuserdata(L, (void *)CTX_REGISTRY_KEY);
    lua_pushlightuserdata(L, ctx);
    lua_settable(L, LUA_REGISTRYINDEX);

    /* Create bot table */
    luaL_newlib(L, bot_funcs);
    lua_setglobal(L, "bot");
}

void tb_strategy_api_set_context(lua_State *L, tb_lua_ctx_t *ctx) {
    lua_pushlightuserdata(L, (void *)CTX_REGISTRY_KEY);
    lua_pushlightuserdata(L, ctx);
    lua_settable(L, LUA_REGISTRYINDEX);
}
