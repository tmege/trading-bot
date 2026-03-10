#include "backtest/backtest_engine.h"
#include "core/logging.h"
#include "strategy/indicators.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

/* ── Lua sandbox limits (same as live engine) ─────────────────────────── */
#define BT_LUA_MAX_INSTRUCTIONS 2000000  /* 2M for backtest (more than live) */
#define BT_LUA_MAX_MEMORY (256 * 1024 * 1024) /* 256MB for backtest (long runs) */

typedef struct { size_t used; size_t limit; } bt_lua_mem_tracker_t;

static void bt_lua_instruction_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    luaL_error(L, "instruction limit exceeded (%d instructions)", BT_LUA_MAX_INSTRUCTIONS);
}

static void *bt_lua_mem_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    bt_lua_mem_tracker_t *tracker = (bt_lua_mem_tracker_t *)ud;
    if (nsize == 0) {
        tracker->used -= osize;
        free(ptr);
        return NULL;
    }
    if (tracker->used - osize + nsize > tracker->limit) return NULL;
    void *new_ptr = realloc(ptr, nsize);
    if (new_ptr) tracker->used = tracker->used - osize + nsize;
    return new_ptr;
}

/* ── ANSI codes ────────────────────────────────────────────────────────── */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_CYAN    "\033[36m"
#define C_WHITE   "\033[37m"

#define LINE_W 72

/* ── Simulated order ───────────────────────────────────────────────────── */
typedef struct {
    uint64_t     oid;
    tb_side_t    side;
    double       price;
    double       size;
    bool         is_trigger;
    double       trigger_px;
    bool         reduce_only;
    tb_tif_t     tif;
    bool         active;
} bt_order_t;

#define BT_MAX_ORDERS 256

/* ── Engine state ──────────────────────────────────────────────────────── */
struct tb_backtest_engine {
    tb_backtest_config_t cfg;

    /* Candle data */
    tb_candle_t *candles;
    int          n_candles;

    /* Simulation state */
    double       balance;
    double       position_size;   /* positive = long, negative = short */
    double       entry_price;
    double       unrealized_pnl;
    double       total_fees;
    double       total_pnl;

    /* Orders */
    bt_order_t   orders[BT_MAX_ORDERS];
    int          n_orders;
    uint64_t     next_oid;

    /* Current candle */
    int          current_idx;
    double       current_mid;
    int64_t      current_time;

    /* Lua state */
    lua_State   *L;

    /* Trade log */
    tb_backtest_result_t *result;
};

/* ── Forward declarations for Lua API ──────────────────────────────────── */
static int bt_lua_place_limit(lua_State *L);
static int bt_lua_place_trigger(lua_State *L);
static int bt_lua_cancel(lua_State *L);
static int bt_lua_cancel_all(lua_State *L);
static int bt_lua_get_position(lua_State *L);
static int bt_lua_get_mid_price(lua_State *L);
static int bt_lua_get_open_orders(lua_State *L);
static int bt_lua_get_account_value(lua_State *L);
static int bt_lua_get_daily_pnl(lua_State *L);
static int bt_lua_get_candles(lua_State *L);
static int bt_lua_get_indicators(lua_State *L);
static int bt_lua_log(lua_State *L);
static int bt_lua_time(lua_State *L);
static int bt_lua_save_state(lua_State *L);
static int bt_lua_load_state(lua_State *L);
static int bt_lua_get_macro(lua_State *L);
static int bt_lua_get_sentiment(lua_State *L);
static int bt_lua_get_fear_greed(lua_State *L);

/* ── Record a trade ────────────────────────────────────────────────────── */
static void record_trade(tb_backtest_engine_t *bt, const char *side,
                         double price, double size, double pnl, double fee) {
    tb_backtest_result_t *r = bt->result;
    if (r->n_trade_log < 4096) {
        int i = r->n_trade_log++;
        r->trades[i].time_ms = bt->current_time;
        strncpy(r->trades[i].side, side, 7);
        r->trades[i].price = price;
        r->trades[i].size = size;
        r->trades[i].pnl = pnl;
        r->trades[i].fee = fee;
        r->trades[i].balance_after = bt->balance;
    }
}

/* ── Execute a fill ────────────────────────────────────────────────────── */
static void execute_fill(tb_backtest_engine_t *bt, tb_side_t side,
                         double price, double size, bool is_taker,
                         uint64_t oid) {
    double fee_rate = is_taker ? bt->cfg.taker_fee_rate : bt->cfg.maker_fee_rate;
    double notional = price * size;
    double fee = notional * fee_rate;
    double pnl = 0;

    /* Apply slippage for taker orders */
    if (is_taker && bt->cfg.slippage_bps > 0) {
        double slip = price * bt->cfg.slippage_bps / 10000.0;
        price += (side == TB_SIDE_BUY) ? slip : -slip;
    }

    double signed_size = (side == TB_SIDE_BUY) ? size : -size;

    if (bt->position_size == 0) {
        /* Opening new position */
        bt->position_size = signed_size;
        bt->entry_price = price;
    } else if ((bt->position_size > 0 && side == TB_SIDE_SELL) ||
               (bt->position_size < 0 && side == TB_SIDE_BUY)) {
        /* Closing or reducing position */
        double close_size = fmin(fabs(signed_size), fabs(bt->position_size));
        if (bt->position_size > 0)
            pnl = (price - bt->entry_price) * close_size;
        else
            pnl = (bt->entry_price - price) * close_size;

        double remaining = fabs(bt->position_size) - close_size;
        if (remaining < 1e-10) {
            /* Fully closed */
            bt->position_size = 0;
            bt->entry_price = 0;
            /* Check if flipping */
            double excess = fabs(signed_size) - close_size;
            if (excess > 1e-10) {
                bt->position_size = (side == TB_SIDE_BUY) ? excess : -excess;
                bt->entry_price = price;
            }
        } else {
            bt->position_size = (bt->position_size > 0) ? remaining : -remaining;
        }
    } else {
        /* Adding to position (average in) */
        double old_notional = fabs(bt->position_size) * bt->entry_price;
        double new_notional = size * price;
        double total_size = fabs(bt->position_size) + size;
        bt->entry_price = (old_notional + new_notional) / total_size;
        bt->position_size += signed_size;
    }

    bt->balance += pnl - fee;
    bt->total_pnl += pnl;
    bt->total_fees += fee;

    record_trade(bt, side == TB_SIDE_BUY ? "BUY" : "SELL",
                 price, size, pnl, fee);

    /* Notify Lua of fill */
    if (bt->L) {
        lua_getglobal(bt->L, "on_fill");
        if (lua_isfunction(bt->L, -1)) {
            lua_newtable(bt->L);
            lua_pushstring(bt->L, bt->cfg.coin);
            lua_setfield(bt->L, -2, "coin");
            lua_pushstring(bt->L, side == TB_SIDE_BUY ? "buy" : "sell");
            lua_setfield(bt->L, -2, "side");
            lua_pushnumber(bt->L, price);
            lua_setfield(bt->L, -2, "price");
            lua_pushnumber(bt->L, size);
            lua_setfield(bt->L, -2, "size");
            lua_pushnumber(bt->L, pnl);
            lua_setfield(bt->L, -2, "pnl");
            lua_pushnumber(bt->L, pnl);
            lua_setfield(bt->L, -2, "closed_pnl");
            lua_pushnumber(bt->L, fee);
            lua_setfield(bt->L, -2, "fee");
            lua_pushinteger(bt->L, (lua_Integer)oid);
            lua_setfield(bt->L, -2, "oid");
            if (lua_pcall(bt->L, 1, 0, 0) != LUA_OK) {
                tb_log_warn("backtest: on_fill error: %s",
                            lua_tostring(bt->L, -1));
                lua_pop(bt->L, 1);
            }
        } else {
            lua_pop(bt->L, 1);
        }
    }
}

/* ── Check limit orders for fills ──────────────────────────────────────── */
static void check_order_fills(tb_backtest_engine_t *bt,
                              double high, double low) {
    for (int i = 0; i < bt->n_orders; i++) {
        bt_order_t *o = &bt->orders[i];
        if (!o->active) continue;

        bool filled = false;
        double fill_price = o->price;

        if (o->is_trigger) {
            /* Trigger order: activate when price crosses trigger_px */
            bool triggered = false;
            if (o->side == TB_SIDE_BUY && high >= o->trigger_px)
                triggered = true;
            if (o->side == TB_SIDE_SELL && low <= o->trigger_px)
                triggered = true;

            if (triggered) {
                fill_price = o->trigger_px;
                filled = true;
            }
        } else {
            /* Limit order: fill when price touches */
            if (o->side == TB_SIDE_BUY && low <= o->price)
                filled = true;
            if (o->side == TB_SIDE_SELL && high >= o->price)
                filled = true;

            /* IOC orders fill at current mid if not immediately fillable */
            if (o->tif == TB_TIF_IOC) {
                if (o->side == TB_SIDE_BUY && bt->current_mid <= o->price)
                    filled = true;
                else if (o->side == TB_SIDE_SELL && bt->current_mid >= o->price)
                    filled = true;

                if (!filled) {
                    o->active = false; /* IOC cancelled */
                    continue;
                }
            }
        }

        if (filled) {
            /* Check reduce_only constraint */
            if (o->reduce_only) {
                if (bt->position_size == 0) { o->active = false; continue; }
                if (o->side == TB_SIDE_BUY && bt->position_size > 0) {
                    o->active = false; continue;
                }
                if (o->side == TB_SIDE_SELL && bt->position_size < 0) {
                    o->active = false; continue;
                }
            }

            bool is_taker = (o->tif == TB_TIF_IOC || o->is_trigger);
            execute_fill(bt, o->side, fill_price, o->size, is_taker, o->oid);
            o->active = false;
        }
    }
}

/* Forward declaration */
static tb_backtest_engine_t *get_bt(lua_State *L);
static void setup_lua_sandbox(tb_backtest_engine_t *bt);

/* ── Synthetic data stubs for backtest ─────────────────────────────────── */

static int bt_lua_get_macro(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "valid");
    /* Simulate BTC price correlated with ETH */
    lua_pushnumber(L, bt->current_mid * 15.0);  /* rough ETH/BTC ratio */
    lua_setfield(L, -2, "btc_price");
    lua_pushnumber(L, 60.0);
    lua_setfield(L, -2, "btc_dominance");
    lua_pushnumber(L, 0.055);
    lua_setfield(L, -2, "eth_btc");
    lua_pushnumber(L, 1200e9);
    lua_setfield(L, -2, "total2_mcap");
    lua_pushnumber(L, 2000.0);
    lua_setfield(L, -2, "gold");
    lua_pushnumber(L, 5400.0);
    lua_setfield(L, -2, "sp500");
    lua_pushnumber(L, 104.0);
    lua_setfield(L, -2, "dxy");
    return 1;
}

static int bt_lua_get_sentiment(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "valid");
    /* Generate pseudo-random sentiment from price momentum */
    double score = 0.0;
    if (bt->current_idx > 5) {
        double prev = tb_decimal_to_double(bt->candles[bt->current_idx - 5].close);
        double curr = bt->current_mid;
        score = (curr - prev) / prev * 10.0;  /* amplified momentum as sentiment */
        if (score > 1.0) score = 1.0;
        if (score < -1.0) score = -1.0;
    }
    lua_pushnumber(L, score);
    lua_setfield(L, -2, "score");
    lua_pushnumber(L, score);
    lua_setfield(L, -2, "overall_score");
    lua_pushnumber(L, score > 0 ? 60.0 : 40.0);
    lua_setfield(L, -2, "bullish_pct");
    lua_pushnumber(L, score > 0 ? 40.0 : 60.0);
    lua_setfield(L, -2, "bearish_pct");
    lua_pushinteger(L, 150);
    lua_setfield(L, -2, "total_tweets");
    return 1;
}

static int bt_lua_get_fear_greed(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "valid");
    /* Simulate Fear & Greed based on recent price action */
    int fg = 50;
    if (bt->current_idx > 20) {
        double prev = tb_decimal_to_double(bt->candles[bt->current_idx - 20].close);
        double change = ((bt->current_mid - prev) / prev) * 100;
        fg = 50 + (int)(change * 5);
        if (fg < 5) fg = 5;
        if (fg > 95) fg = 95;
    }
    lua_pushinteger(L, fg);
    lua_setfield(L, -2, "value");
    const char *label = fg < 25 ? "Extreme Fear" : fg < 45 ? "Fear" :
                        fg < 55 ? "Neutral" : fg < 75 ? "Greed" : "Extreme Greed";
    lua_pushstring(L, label);
    lua_setfield(L, -2, "label");
    return 1;
}

/* ── Lua sandbox setup ─────────────────────────────────────────────────── */
static void setup_lua_sandbox(tb_backtest_engine_t *bt) {
    lua_State *L = bt->L;

    /* Remove dangerous modules */
    lua_pushnil(L); lua_setglobal(L, "io");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "require");
    lua_pushnil(L); lua_setglobal(L, "debug");
    lua_pushnil(L); lua_setglobal(L, "package");
    lua_pushnil(L); lua_setglobal(L, "load");
    lua_pushnil(L); lua_setglobal(L, "loadstring");
    lua_pushnil(L); lua_setglobal(L, "rawget");
    lua_pushnil(L); lua_setglobal(L, "rawset");
    lua_pushnil(L); lua_setglobal(L, "setmetatable");
    lua_pushnil(L); lua_setglobal(L, "getmetatable");
    /* collectgarbage is allowed in backtest (needed for long runs) */

    /* Remove string.dump */
    lua_getglobal(L, "string");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
    }
    lua_pop(L, 1);

    /* Restrict os: keep only clock, time, difftime */
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "clock");
        lua_getfield(L, -2, "time");
        lua_getfield(L, -3, "difftime");
        lua_newtable(L);
        lua_pushvalue(L, -4); lua_setfield(L, -2, "clock");
        lua_pushvalue(L, -4); lua_setfield(L, -2, "time");
        lua_pushvalue(L, -4); lua_setfield(L, -2, "difftime");
        lua_setglobal(L, "os");
        lua_pop(L, 4);
    } else {
        lua_pop(L, 1);
    }

    /* Register bot.* API */
    lua_newtable(L);

    /* Store engine pointer in registry */
    lua_pushlightuserdata(L, bt);
    lua_setfield(L, LUA_REGISTRYINDEX, "bt_engine");

    static const luaL_Reg bot_funcs[] = {
        {"place_limit",      bt_lua_place_limit},
        {"place_trigger",    bt_lua_place_trigger},
        {"cancel",           bt_lua_cancel},
        {"cancel_all",       bt_lua_cancel_all},
        {"get_position",     bt_lua_get_position},
        {"get_mid_price",    bt_lua_get_mid_price},
        {"get_open_orders",  bt_lua_get_open_orders},
        {"get_account_value",bt_lua_get_account_value},
        {"get_daily_pnl",    bt_lua_get_daily_pnl},
        {"get_candles",      bt_lua_get_candles},
        {"get_indicators",   bt_lua_get_indicators},
        {"get_macro",        bt_lua_get_macro},
        {"get_sentiment",    bt_lua_get_sentiment},
        {"get_fear_greed",   bt_lua_get_fear_greed},
        {"log",              bt_lua_log},
        {"time",             bt_lua_time},
        {"save_state",       bt_lua_save_state},
        {"load_state",       bt_lua_load_state},
        {NULL, NULL}
    };

    luaL_setfuncs(L, bot_funcs, 0);
    lua_setglobal(L, "bot");
}

static tb_backtest_engine_t *get_bt(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "bt_engine");
    tb_backtest_engine_t *bt = (tb_backtest_engine_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return bt;
}

/* ── Lua bot.* implementations ─────────────────────────────────────────── */

static int bt_lua_place_limit(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    /* bot.place_limit(coin, side, price, size, opts) */
    const char *side_str = luaL_checkstring(L, 2);
    double price = luaL_checknumber(L, 3);
    double size = luaL_checknumber(L, 4);

    tb_side_t side = (strcmp(side_str, "BUY") == 0 || strcmp(side_str, "buy") == 0 ||
                      strcmp(side_str, "B") == 0)
                     ? TB_SIDE_BUY : TB_SIDE_SELL;

    tb_tif_t tif = TB_TIF_GTC;
    bool reduce_only = false;
    if (lua_istable(L, 5)) {
        lua_getfield(L, 5, "tif");
        if (lua_isstring(L, -1)) {
            const char *t = lua_tostring(L, -1);
            if (strcmp(t, "IOC") == 0) tif = TB_TIF_IOC;
            else if (strcmp(t, "ALO") == 0) tif = TB_TIF_ALO;
        }
        lua_pop(L, 1);
        lua_getfield(L, 5, "reduce_only");
        if (lua_isboolean(L, -1)) reduce_only = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < BT_MAX_ORDERS; i++) {
        if (!bt->orders[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        lua_pushnil(L);
        return 1;
    }

    bt->orders[slot] = (bt_order_t){
        .oid = bt->next_oid++,
        .side = side,
        .price = price,
        .size = size,
        .is_trigger = false,
        .reduce_only = reduce_only,
        .tif = tif,
        .active = true,
    };
    if (slot >= bt->n_orders) bt->n_orders = slot + 1;

    lua_pushinteger(L, (lua_Integer)bt->orders[slot].oid);
    return 1;
}

static int bt_lua_place_trigger(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    /* bot.place_trigger(coin, side, trigger_px, size, opts) */
    const char *side_str = luaL_checkstring(L, 2);
    double trigger_px = luaL_checknumber(L, 3);
    double size = luaL_checknumber(L, 4);

    tb_side_t side = (strcmp(side_str, "BUY") == 0 || strcmp(side_str, "buy") == 0 ||
                      strcmp(side_str, "B") == 0)
                     ? TB_SIDE_BUY : TB_SIDE_SELL;

    bool reduce_only = false;
    if (lua_istable(L, 5)) {
        lua_getfield(L, 5, "reduce_only");
        if (lua_isboolean(L, -1)) reduce_only = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    int slot = -1;
    for (int i = 0; i < BT_MAX_ORDERS; i++) {
        if (!bt->orders[i].active) { slot = i; break; }
    }
    if (slot < 0) { lua_pushnil(L); return 1; }

    bt->orders[slot] = (bt_order_t){
        .oid = bt->next_oid++,
        .side = side,
        .price = trigger_px,
        .size = size,
        .is_trigger = true,
        .trigger_px = trigger_px,
        .reduce_only = reduce_only,
        .tif = TB_TIF_GTC,
        .active = true,
    };
    if (slot >= bt->n_orders) bt->n_orders = slot + 1;

    lua_pushinteger(L, (lua_Integer)bt->orders[slot].oid);
    return 1;
}

static int bt_lua_cancel(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    /* bot.cancel(coin, oid) */
    uint64_t oid = (uint64_t)luaL_checkinteger(L, 2);
    for (int i = 0; i < bt->n_orders; i++) {
        if (bt->orders[i].active && bt->orders[i].oid == oid) {
            bt->orders[i].active = false;
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int bt_lua_cancel_all(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    int cancelled = 0;
    for (int i = 0; i < bt->n_orders; i++) {
        if (bt->orders[i].active) {
            bt->orders[i].active = false;
            cancelled++;
        }
    }
    lua_pushinteger(L, cancelled);
    return 1;
}

static int bt_lua_get_position(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    lua_newtable(L);
    lua_pushstring(L, bt->cfg.coin);
    lua_setfield(L, -2, "coin");
    lua_pushnumber(L, bt->position_size);
    lua_setfield(L, -2, "size");
    lua_pushnumber(L, bt->entry_price);
    lua_setfield(L, -2, "entry_px");
    lua_pushnumber(L, bt->unrealized_pnl);
    lua_setfield(L, -2, "unrealized_pnl");
    return 1;
}

static int bt_lua_get_mid_price(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    lua_pushnumber(L, bt->current_mid);
    return 1;
}

static int bt_lua_get_open_orders(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    lua_newtable(L);
    int n = 0;
    for (int i = 0; i < bt->n_orders; i++) {
        if (!bt->orders[i].active) continue;
        n++;
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)bt->orders[i].oid);
        lua_setfield(L, -2, "oid");
        lua_pushstring(L, bt->orders[i].side == TB_SIDE_BUY ? "BUY" : "SELL");
        lua_setfield(L, -2, "side");
        lua_pushnumber(L, bt->orders[i].price);
        lua_setfield(L, -2, "price");
        lua_pushnumber(L, bt->orders[i].size);
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, n);
    }
    return 1;
}

static int bt_lua_get_account_value(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    lua_pushnumber(L, bt->balance + bt->unrealized_pnl);
    return 1;
}

static int bt_lua_get_daily_pnl(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    lua_pushnumber(L, bt->total_pnl - bt->total_fees);
    return 1;
}

static int bt_lua_get_candles(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    /* bot.get_candles(coin, interval, limit) */
    int limit = 100;
    if (lua_isnumber(L, 3)) limit = (int)lua_tointeger(L, 3);

    int start = bt->current_idx - limit + 1;
    if (start < 0) start = 0;

    lua_newtable(L);
    int n = 0;
    for (int i = start; i <= bt->current_idx; i++) {
        n++;
        lua_newtable(L);
        lua_pushnumber(L, tb_decimal_to_double(bt->candles[i].open));
        lua_setfield(L, -2, "open");
        lua_pushnumber(L, tb_decimal_to_double(bt->candles[i].high));
        lua_setfield(L, -2, "high");
        lua_pushnumber(L, tb_decimal_to_double(bt->candles[i].low));
        lua_setfield(L, -2, "low");
        lua_pushnumber(L, tb_decimal_to_double(bt->candles[i].close));
        lua_setfield(L, -2, "close");
        lua_pushnumber(L, tb_decimal_to_double(bt->candles[i].volume));
        lua_setfield(L, -2, "volume");
        lua_pushnumber(L, tb_decimal_to_double(bt->candles[i].volume));
        lua_setfield(L, -2, "v");
        lua_pushinteger(L, bt->candles[i].time_open);
        lua_setfield(L, -2, "time");
        lua_rawseti(L, -2, n);
    }
    return 1;
}

static int bt_lua_get_indicators(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    /* bot.get_indicators(coin, interval, limit) */
    int limit = 250;
    if (lua_isnumber(L, 3)) limit = (int)lua_tointeger(L, 3);

    int start = bt->current_idx - limit + 1;
    if (start < 0) start = 0;
    int n = bt->current_idx - start + 1;

    tb_candle_input_t *inputs = malloc(sizeof(tb_candle_input_t) * n);
    if (!inputs) { lua_pushnil(L); return 1; }

    for (int i = 0; i < n; i++) {
        int ci = start + i;
        inputs[i].open   = tb_decimal_to_double(bt->candles[ci].open);
        inputs[i].high   = tb_decimal_to_double(bt->candles[ci].high);
        inputs[i].low    = tb_decimal_to_double(bt->candles[ci].low);
        inputs[i].close  = tb_decimal_to_double(bt->candles[ci].close);
        inputs[i].volume = tb_decimal_to_double(bt->candles[ci].volume);
        inputs[i].time_ms = bt->candles[ci].time_open;
    }

    tb_indicators_snapshot_t snap = tb_indicators_compute(inputs, n);
    free(inputs);

    lua_newtable(L);
    lua_pushnumber(L, snap.sma_20);   lua_setfield(L, -2, "sma_20");
    lua_pushnumber(L, snap.sma_50);   lua_setfield(L, -2, "sma_50");
    lua_pushnumber(L, snap.sma_200);  lua_setfield(L, -2, "sma_200");
    lua_pushnumber(L, snap.ema_12);   lua_setfield(L, -2, "ema_12");
    lua_pushnumber(L, snap.ema_26);   lua_setfield(L, -2, "ema_26");
    lua_pushnumber(L, snap.rsi_14);   lua_setfield(L, -2, "rsi_14");
    lua_pushnumber(L, snap.macd_line);      lua_setfield(L, -2, "macd_line");
    lua_pushnumber(L, snap.macd_signal);    lua_setfield(L, -2, "macd_signal");
    lua_pushnumber(L, snap.macd_histogram); lua_setfield(L, -2, "macd_histogram");
    lua_pushnumber(L, snap.bb_upper);  lua_setfield(L, -2, "bb_upper");
    lua_pushnumber(L, snap.bb_middle); lua_setfield(L, -2, "bb_middle");
    lua_pushnumber(L, snap.bb_lower);  lua_setfield(L, -2, "bb_lower");
    lua_pushnumber(L, snap.bb_width);  lua_setfield(L, -2, "bb_width");
    lua_pushnumber(L, snap.atr_14);    lua_setfield(L, -2, "atr_14");
    lua_pushnumber(L, snap.vwap);      lua_setfield(L, -2, "vwap");

    /* Aliases for strategy compatibility */
    lua_pushnumber(L, snap.sma_20);    lua_setfield(L, -2, "sma");
    lua_pushnumber(L, snap.ema_12);    lua_setfield(L, -2, "ema");
    lua_pushnumber(L, snap.ema_12);    lua_setfield(L, -2, "ema_fast");
    lua_pushnumber(L, snap.ema_26);    lua_setfield(L, -2, "ema_slow");
    lua_pushnumber(L, snap.rsi_14);    lua_setfield(L, -2, "rsi");
    lua_pushnumber(L, snap.atr_14);    lua_setfield(L, -2, "atr");
    lua_pushnumber(L, snap.bb_middle); lua_setfield(L, -2, "bb_mid");

    lua_pushboolean(L, snap.above_sma_200);     lua_setfield(L, -2, "above_sma_200");
    lua_pushboolean(L, snap.golden_cross);      lua_setfield(L, -2, "golden_cross");
    lua_pushboolean(L, snap.rsi_oversold);      lua_setfield(L, -2, "rsi_oversold");
    lua_pushboolean(L, snap.rsi_overbought);    lua_setfield(L, -2, "rsi_overbought");
    lua_pushboolean(L, snap.bb_squeeze);        lua_setfield(L, -2, "bb_squeeze");
    lua_pushboolean(L, snap.macd_bullish_cross);lua_setfield(L, -2, "macd_bullish_cross");

    return 1;
}

static int bt_lua_log(lua_State *L) {
    /* bot.log(level, msg) — show warnings only */
    const char *level = lua_tostring(L, 1);
    const char *msg = lua_tostring(L, 2);
    if (level && msg && (strcmp(level, "warn") == 0 || strcmp(level, "info") == 0)) {
        tb_log_debug("bt-lua [%s]: %s", level, msg);
    }
    return 0;
}

static int bt_lua_time(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    lua_pushinteger(L, bt->current_time);
    return 1;
}

static int bt_lua_save_state(lua_State *L) {
    /* Store in Lua registry */
    tb_backtest_engine_t *bt = get_bt(L);
    const char *key = luaL_checkstring(L, 1);
    char regkey[128];
    snprintf(regkey, sizeof(regkey), "bt_state_%s", key);
    lua_pushvalue(L, 2);
    lua_setfield(bt->L, LUA_REGISTRYINDEX, regkey);
    lua_pushboolean(L, 1);
    return 1;
}

static int bt_lua_load_state(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    const char *key = luaL_checkstring(L, 1);
    char regkey[128];
    snprintf(regkey, sizeof(regkey), "bt_state_%s", key);
    lua_getfield(bt->L, LUA_REGISTRYINDEX, regkey);
    return 1;
}

/* ── Public API ────────────────────────────────────────────────────────── */

tb_backtest_engine_t *tb_backtest_create(const tb_backtest_config_t *cfg) {
    tb_backtest_engine_t *bt = calloc(1, sizeof(tb_backtest_engine_t));
    if (!bt) return NULL;
    bt->cfg = *cfg;
    bt->balance = cfg->initial_balance;
    bt->next_oid = 1;

    /* Defaults */
    if (bt->cfg.maker_fee_rate == 0) bt->cfg.maker_fee_rate = 0.0002;
    if (bt->cfg.taker_fee_rate == 0) bt->cfg.taker_fee_rate = 0.0005;

    return bt;
}

static void bt_free_lua_state(lua_State *L) {
    if (!L) return;
    lua_getfield(L, LUA_REGISTRYINDEX, "bt_mem_tracker");
    void *tracker = lua_touserdata(L, -1);
    lua_pop(L, 1);
    lua_close(L);
    free(tracker);
}

void tb_backtest_destroy(tb_backtest_engine_t *bt) {
    if (!bt) return;
    bt_free_lua_state(bt->L);
    bt->L = NULL;
    free(bt->candles);
    free(bt);
}

int tb_backtest_load_candles(tb_backtest_engine_t *bt,
                             const tb_candle_t *candles, int n_candles) {
    free(bt->candles);
    bt->candles = malloc(sizeof(tb_candle_t) * n_candles);
    if (!bt->candles) return -1;
    memcpy(bt->candles, candles, sizeof(tb_candle_t) * n_candles);
    bt->n_candles = n_candles;
    return 0;
}

int tb_backtest_run(tb_backtest_engine_t *bt, tb_backtest_result_t *out) {
    if (!bt->candles || bt->n_candles == 0) {
        tb_log_warn("backtest: no candles loaded");
        return -1;
    }

    memset(out, 0, sizeof(*out));
    bt->result = out;
    out->start_balance = bt->balance;
    out->start_time_ms = bt->candles[0].time_open;
    out->end_time_ms = bt->candles[bt->n_candles - 1].time_close;
    out->n_candles = bt->n_candles;

    /* Calculate days */
    out->n_days = (int)((out->end_time_ms - out->start_time_ms) /
                        (86400LL * 1000)) + 1;

    /* Initialize Lua with memory limits */
    bt_lua_mem_tracker_t *tracker = calloc(1, sizeof(bt_lua_mem_tracker_t));
    if (!tracker) return -1;
    tracker->limit = BT_LUA_MAX_MEMORY;
    bt->L = lua_newstate(bt_lua_mem_alloc, tracker, 0);
    if (!bt->L) { free(tracker); return -1; }
    luaL_openlibs(bt->L);
    /* Install instruction limit hook */
    lua_sethook(bt->L, bt_lua_instruction_hook, LUA_MASKCOUNT, BT_LUA_MAX_INSTRUCTIONS);
    /* Store tracker for cleanup */
    lua_pushlightuserdata(bt->L, tracker);
    lua_setfield(bt->L, LUA_REGISTRYINDEX, "bt_mem_tracker");
    setup_lua_sandbox(bt);

    /* Load strategy */
    if (luaL_dofile(bt->L, bt->cfg.strategy_path) != LUA_OK) {
        tb_log_warn("backtest: failed to load strategy: %s",
                    lua_tostring(bt->L, -1));
        bt_free_lua_state(bt->L);
        bt->L = NULL;
        return -1;
    }

    /* Call on_init */
    lua_getglobal(bt->L, "on_init");
    if (lua_isfunction(bt->L, -1)) {
        lua_newtable(bt->L);
        lua_pushstring(bt->L, "backtest");
        lua_setfield(bt->L, -2, "mode");
        if (lua_pcall(bt->L, 1, 0, 0) != LUA_OK) {
            tb_log_warn("backtest: on_init error: %s",
                        lua_tostring(bt->L, -1));
            lua_pop(bt->L, 1);
        }
    } else {
        lua_pop(bt->L, 1);
    }

    /* Equity tracking */
    double peak_equity = bt->balance;
    double max_dd = 0, max_dd_pct = 0;
    int64_t last_equity_day = 0;

    /* Main simulation loop */
    for (int i = 0; i < bt->n_candles; i++) {
        bt->current_idx = i;
        double close = tb_decimal_to_double(bt->candles[i].close);
        double high = tb_decimal_to_double(bt->candles[i].high);
        double low = tb_decimal_to_double(bt->candles[i].low);
        bt->current_mid = close;
        bt->current_time = bt->candles[i].time_open;

        /* Update unrealized P&L */
        if (bt->position_size != 0) {
            if (bt->position_size > 0)
                bt->unrealized_pnl = (close - bt->entry_price) * bt->position_size;
            else
                bt->unrealized_pnl = (bt->entry_price - close) * fabs(bt->position_size);
        } else {
            bt->unrealized_pnl = 0;
        }

        /* Check order fills (use high/low of candle) */
        check_order_fills(bt, high, low);

        /* Periodic GC to prevent memory exhaustion in long backtests */
        if (i % 10 == 0) lua_gc(bt->L, LUA_GCSTEP, 100);

        /* Call on_tick(coin, mid_price) */
        lua_getglobal(bt->L, "on_tick");
        if (lua_isfunction(bt->L, -1)) {
            lua_pushstring(bt->L, bt->cfg.coin);
            lua_pushnumber(bt->L, close);
            if (lua_pcall(bt->L, 2, 0, 0) != LUA_OK) {
                tb_log_warn("backtest: on_tick error at candle %d: %s",
                            i, lua_tostring(bt->L, -1));
                lua_pop(bt->L, 1);
            }
        } else {
            lua_pop(bt->L, 1);
        }

        /* Call on_timer every 60 candles (simulate periodic timer) */
        if (i > 0 && i % 60 == 0) {
            lua_getglobal(bt->L, "on_timer");
            if (lua_isfunction(bt->L, -1)) {
                if (lua_pcall(bt->L, 0, 0, 0) != LUA_OK) {
                    lua_pop(bt->L, 1);
                }
            } else {
                lua_pop(bt->L, 1);
            }
        }

        /* Track equity & drawdown */
        double equity = bt->balance + bt->unrealized_pnl;
        if (equity > peak_equity) peak_equity = equity;
        double dd = peak_equity - equity;
        if (dd > max_dd) max_dd = dd;
        double dd_pct = peak_equity > 0 ? dd / peak_equity * 100.0 : 0;
        if (dd_pct > max_dd_pct) max_dd_pct = dd_pct;

        /* Record equity curve (daily) */
        int64_t day = bt->current_time / (86400LL * 1000);
        if (day != last_equity_day && out->n_equity_points < 1000) {
            int ei = out->n_equity_points++;
            out->equity_curve[ei].time_ms = bt->current_time;
            out->equity_curve[ei].equity = equity;
            out->equity_curve[ei].drawdown = dd;
            last_equity_day = day;
        }
    }

    /* Call on_shutdown */
    lua_getglobal(bt->L, "on_shutdown");
    if (lua_isfunction(bt->L, -1)) {
        if (lua_pcall(bt->L, 0, 0, 0) != LUA_OK)
            lua_pop(bt->L, 1);
    } else {
        lua_pop(bt->L, 1);
    }

    /* Close any remaining position at last close */
    if (bt->position_size != 0) {
        double last_close = tb_decimal_to_double(
            bt->candles[bt->n_candles - 1].close);
        tb_side_t close_side = bt->position_size > 0 ? TB_SIDE_SELL : TB_SIDE_BUY;
        execute_fill(bt, close_side, last_close, fabs(bt->position_size), true, 0);
    }

    /* Fill results */
    out->end_balance = bt->balance;
    out->total_pnl = bt->total_pnl;
    out->total_fees = bt->total_fees;
    out->net_pnl = bt->total_pnl - bt->total_fees;
    out->return_pct = out->start_balance > 0 ?
        out->net_pnl / out->start_balance * 100.0 : 0;
    out->max_drawdown = max_dd;
    out->max_drawdown_pct = max_dd_pct;

    /* Trade stats */
    double gross_profit = 0, gross_loss = 0;
    double sum_returns = 0;
    double sum_neg_sq = 0;
    int n_returns = 0;

    for (int i = 0; i < out->n_trade_log; i++) {
        double pnl = out->trades[i].pnl;
        if (pnl > 0) {
            out->winning_trades++;
            gross_profit += pnl;
            if (pnl > out->max_win) out->max_win = pnl;
        } else if (pnl < 0) {
            out->losing_trades++;
            gross_loss += -pnl;
            if (pnl < out->max_loss) out->max_loss = pnl;
        }
        /* For Sharpe/Sortino, use per-trade return */
        double ret = out->start_balance > 0 ? pnl / out->start_balance : 0;
        sum_returns += ret;
        if (ret < 0) sum_neg_sq += ret * ret;
        n_returns++;
    }

    out->total_trades = out->n_trade_log;
    if (out->total_trades > 0)
        out->win_rate = (double)out->winning_trades / out->total_trades * 100.0;
    if (gross_loss > 0)
        out->profit_factor = gross_profit / gross_loss;
    else if (gross_profit > 0)
        out->profit_factor = 9999.0;  /* no losses = infinite PF */
    if (out->winning_trades > 0)
        out->avg_win = gross_profit / out->winning_trades;
    if (out->losing_trades > 0)
        out->avg_loss = -gross_loss / out->losing_trades;

    /* Sharpe & Sortino (annualized) */
    if (n_returns > 1 && out->n_days > 0) {
        double mean_ret = sum_returns / n_returns;
        double sum_sq = 0;
        for (int i = 0; i < out->n_trade_log; i++) {
            double ret = out->start_balance > 0 ?
                out->trades[i].pnl / out->start_balance : 0;
            sum_sq += (ret - mean_ret) * (ret - mean_ret);
        }
        double std_dev = sqrt(sum_sq / (n_returns - 1));
        double ann_factor = sqrt(365.0 / out->n_days * n_returns);
        if (std_dev > 0)
            out->sharpe_ratio = mean_ret / std_dev * ann_factor;

        double downside_dev = sqrt(sum_neg_sq / n_returns);
        if (downside_dev > 0)
            out->sortino_ratio = mean_ret / downside_dev * ann_factor;
        else if (mean_ret > 0)
            out->sortino_ratio = 9999.0;  /* no downside = infinite Sortino */
    }

    bt_free_lua_state(bt->L);
    bt->L = NULL;

    return 0;
}

/* ── Print results ─────────────────────────────────────────────────────── */
void tb_backtest_print_results(const tb_backtest_result_t *r) {
    printf("\n");
    printf(C_BOLD C_WHITE "  BACKTEST RESULTS" C_RESET "\n");
    for (int i = 0; i < LINE_W; i++) printf(C_DIM "─" C_RESET);
    printf("\n");

    printf(C_BOLD C_CYAN "  PERFORMANCE" C_RESET "\n");
    const char *pnl_c = r->net_pnl >= 0 ? C_GREEN : C_RED;
    printf("  Balance:     $%.2f → $%.2f\n", r->start_balance, r->end_balance);
    printf("  Net P&L:     %s%+.4f USDC" C_RESET " (%.2f%%)\n",
           pnl_c, r->net_pnl, r->return_pct);
    printf("  Gross P&L:   %+.4f USDC    Fees: -%.4f USDC\n", r->total_pnl, r->total_fees);
    printf("  Period:      %d candles, %d days\n", r->n_candles, r->n_days);
    printf("\n");

    printf(C_BOLD C_CYAN "  TRADE STATS" C_RESET "\n");
    printf("  Total:       %d trades (W:%d L:%d)\n",
           r->total_trades, r->winning_trades, r->losing_trades);
    printf("  Win rate:    %.1f%%\n", r->win_rate);
    printf("  PF:          %.2f\n", r->profit_factor);
    printf("  Avg win:     %s+%.4f USDC" C_RESET "\n", C_GREEN, r->avg_win);
    printf("  Avg loss:    %s%.4f USDC" C_RESET "\n", C_RED, r->avg_loss);
    printf("  Max win:     %s+%.4f USDC" C_RESET "\n", C_GREEN, r->max_win);
    printf("  Max loss:    %s%.4f USDC" C_RESET "\n", C_RED, r->max_loss);
    printf("\n");

    printf(C_BOLD C_CYAN "  RISK METRICS" C_RESET "\n");
    printf("  Max DD:      %s-%.4f USDC" C_RESET " (%.2f%%)\n",
           C_RED, r->max_drawdown, r->max_drawdown_pct);
    printf("  Sharpe:      %.2f\n", r->sharpe_ratio);
    printf("  Sortino:     %.2f\n", r->sortino_ratio);

    for (int i = 0; i < LINE_W; i++) printf(C_DIM "─" C_RESET);
    printf("\n\n");
}
