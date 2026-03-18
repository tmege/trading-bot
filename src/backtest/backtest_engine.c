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

/* ── Lua sandbox limits ────────────────────────────────────────────────── */
/* No instruction limit or memory limit for backtests: they run fork-isolated,
   so the child process has its own virtual address space. The OS reclaims all
   memory when the child exits. The custom allocator had subtle accounting bugs
   (osize = type tag for new allocations) causing false OOM at ~2500 candles. */

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
    int          placed_at_idx;  /* 5m candle index when order was placed */
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

    /* Current 5m candle */
    int          current_idx;      /* index into candles[] (5m) */
    double       current_mid;
    int64_t      current_time;

    /* Multi-timeframe aggregation (5m → strategy TF) */
    tb_candle_t *candles_tf;       /* aggregated candles (1h, 4h...) */
    int          n_candles_tf;     /* number of completed TF candles */
    int          candles_tf_cap;   /* allocated capacity */
    int64_t      tf_interval_ms;   /* strategy interval in ms */
    int          current_tf_idx;   /* current TF index for get_indicators */

    /* Partial TF candle being aggregated */
    int64_t      partial_tf_start; /* time_open of current TF candle */
    double       partial_open;
    double       partial_high;
    double       partial_low;
    double       partial_close;
    double       partial_volume;
    int          partial_count;    /* number of 5m candles in partial */

    /* Lua state */
    lua_State   *L;

    /* Trade log */
    tb_backtest_result_t *result;

    /* Running stat accumulators (not capped by trade log size) */
    int    stat_total;
    int    stat_wins;
    int    stat_losses;
    double stat_gross_profit;
    double stat_gross_loss;
    double stat_max_win;
    double stat_max_loss;
    /* Welford online algo for Sharpe variance */
    double stat_sum_ret;
    double stat_sum_ret_m2;     /* sum of squared deviations from running mean */
    double stat_sum_neg_sq;     /* for Sortino downside deviation */
    /* Daily P&L tracking (resets at UTC day boundary like live) */
    int64_t current_day;        /* current UTC day number */
    double  day_start_equity;   /* equity at start of current day */
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
static int bt_lua_get_sentiment(lua_State *L);
static int bt_lua_get_fear_greed(lua_State *L);
/* ── Record a trade ────────────────────────────────────────────────────── */
static void record_trade(tb_backtest_engine_t *bt, const char *side,
                         double price, double size, double pnl, double fee) {
    tb_backtest_result_t *r = bt->result;
    /* Grow trade log dynamically */
    if (r->n_trade_log >= r->trade_cap) {
        int new_cap = r->trade_cap == 0 ? 4096 : r->trade_cap * 2;
        tb_bt_trade_t *new_trades = realloc(r->trades, (size_t)new_cap * sizeof(tb_bt_trade_t));
        if (new_trades) {
            r->trades = new_trades;
            r->trade_cap = new_cap;
        }
    }
    if (r->n_trade_log < r->trade_cap) {
        int i = r->n_trade_log++;
        r->trades[i].time_ms = bt->current_time;
        snprintf(r->trades[i].side, sizeof(r->trades[i].side), "%s", side);
        r->trades[i].price = price;
        r->trades[i].size = size;
        r->trades[i].pnl = pnl;
        r->trades[i].fee = fee;
        r->trades[i].balance_after = bt->balance;
    }

    /* Accumulate stats only on closing trades (pnl != 0).
     * Entry fills have pnl=0 and would dilute win rate, Sharpe, Sortino. */
    if (pnl != 0) {
        bt->stat_total++;
        if (pnl > 0) {
            bt->stat_wins++;
            bt->stat_gross_profit += pnl;
            if (pnl > bt->stat_max_win) bt->stat_max_win = pnl;
        } else {
            bt->stat_losses++;
            bt->stat_gross_loss += -pnl;
            if (pnl < bt->stat_max_loss) bt->stat_max_loss = pnl;
        }
        /* Welford online algorithm for Sharpe variance */
        double ret = bt->cfg.initial_balance > 0 ? pnl / bt->cfg.initial_balance : 0;
        int n = bt->stat_total; /* already incremented above */
        double old_mean = (n > 1) ? bt->stat_sum_ret / (n - 1) : 0.0;
        bt->stat_sum_ret += ret;
        double new_mean = bt->stat_sum_ret / n;
        bt->stat_sum_ret_m2 += (ret - old_mean) * (ret - new_mean);
        if (ret < 0) bt->stat_sum_neg_sq += ret * ret;
    }
}

/* ── Execute a fill ────────────────────────────────────────────────────── */
static void execute_fill(tb_backtest_engine_t *bt, tb_side_t side,
                         double price, double size, bool is_taker,
                         uint64_t oid) {
    /* Apply slippage before fee calculation */
    if (is_taker && bt->cfg.slippage_bps > 0) {
        double slip = price * bt->cfg.slippage_bps / 10000.0;
        price += (side == TB_SIDE_BUY) ? slip : -slip;
    }

    double fee_rate = is_taker ? bt->cfg.taker_fee_rate : bt->cfg.maker_fee_rate;
    double notional = price * size;
    double fee = notional * fee_rate;
    double pnl = 0;

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
/* Two-pass fill: SL triggers first (conservative), then TP/entries.
 * This prevents same-candle SL+TP ambiguity from biasing toward TP. */
static void check_order_fills(tb_backtest_engine_t *bt,
                              double high, double low) {
    /* Pass 1: fill SL/adverse triggers first */
    for (int i = 0; i < bt->n_orders; i++) {
        bt_order_t *o = &bt->orders[i];
        if (!o->active || !o->is_trigger || !o->reduce_only) continue;
        if (o->placed_at_idx >= bt->current_idx) continue;
        if (bt->position_size == 0) continue;

        /* Only fill adverse triggers (SL) in pass 1 */
        bool is_sl = false;
        if (o->side == TB_SIDE_SELL && o->trigger_px <= bt->entry_price)
            is_sl = true; /* Long SL */
        if (o->side == TB_SIDE_BUY && o->trigger_px >= bt->entry_price)
            is_sl = true; /* Short SL */
        if (!is_sl) continue;

        bool triggered = false;
        if (o->side == TB_SIDE_SELL)
            triggered = (low <= o->trigger_px);
        else
            triggered = (high >= o->trigger_px);

        if (triggered) {
            if (o->reduce_only) {
                if (bt->position_size == 0) { o->active = false; continue; }
                if (o->side == TB_SIDE_BUY && bt->position_size > 0) {
                    o->active = false; continue;
                }
                if (o->side == TB_SIDE_SELL && bt->position_size < 0) {
                    o->active = false; continue;
                }
            }
            o->active = false;  /* Deactivate BEFORE execute_fill to prevent slot reuse corruption */
            execute_fill(bt, o->side, o->trigger_px, o->size, true, o->oid);
        }
    }

    /* Pass 2: fill everything else (TP triggers, limit orders, entry triggers) */
    for (int i = 0; i < bt->n_orders; i++) {
        bt_order_t *o = &bt->orders[i];
        if (!o->active) continue;

        /* Skip orders placed on this same 5m candle (prevents same-candle fill) */
        if (o->placed_at_idx >= bt->current_idx) continue;

        bool filled = false;
        double fill_price = o->price;

        if (o->is_trigger) {
            /* Trigger order: direction depends on whether it's SL or TP.
             * For reduce_only orders with an open position, infer from
             * trigger_px vs entry_price:
             *   Long  TP (sell above entry) → fires on high >= trigger_px
             *   Long  SL (sell below entry) → fires on low  <= trigger_px
             *   Short TP (buy below entry)  → fires on low  <= trigger_px
             *   Short SL (buy above entry)  → fires on high >= trigger_px
             * For entry triggers, use standard stop logic. */
            bool triggered = false;

            if (o->reduce_only && bt->position_size != 0) {
                bool is_long = (bt->position_size > 0);
                if (o->side == TB_SIDE_SELL) {
                    if (o->trigger_px > bt->entry_price) {
                        /* Long TP: sell when price rises to TP */
                        triggered = (high >= o->trigger_px);
                    } else {
                        /* Long SL: sell when price drops to SL */
                        triggered = (low <= o->trigger_px);
                    }
                } else { /* TB_SIDE_BUY */
                    if (o->trigger_px < bt->entry_price) {
                        /* Short TP: buy when price drops to TP */
                        triggered = (low <= o->trigger_px);
                    } else {
                        /* Short SL: buy when price rises to SL */
                        triggered = (high >= o->trigger_px);
                    }
                }
                (void)is_long; /* used for clarity above */
            } else {
                /* Entry triggers / non-reduce_only: standard stop logic */
                if (o->side == TB_SIDE_BUY && high >= o->trigger_px)
                    triggered = true;
                if (o->side == TB_SIDE_SELL && low <= o->trigger_px)
                    triggered = true;
            }

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

            /* IOC orders: fill at market price (current_mid) if within limit */
            if (o->tif == TB_TIF_IOC) {
                if (o->side == TB_SIDE_BUY && bt->current_mid <= o->price) {
                    filled = true;
                    fill_price = bt->current_mid;
                } else if (o->side == TB_SIDE_SELL && bt->current_mid >= o->price) {
                    filled = true;
                    fill_price = bt->current_mid;
                }

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
            o->active = false;  /* Deactivate BEFORE execute_fill to prevent slot reuse corruption */
            execute_fill(bt, o->side, fill_price, o->size, is_taker, o->oid);
        }
    }
}

/* Forward declaration */
static tb_backtest_engine_t *get_bt(lua_State *L);
static void setup_lua_sandbox(tb_backtest_engine_t *bt);

/* ── Synthetic data stubs for backtest ─────────────────────────────────── */

static int bt_lua_get_sentiment(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "valid");
    /* Generate pseudo-random sentiment from price momentum (use TF candles) */
    double score = 0.0;
    int use_tf = (bt->candles_tf && bt->n_candles_tf > 0);
    int idx = use_tf ? bt->current_tf_idx : bt->current_idx;
    if (idx > 5) {
        double prev = use_tf
            ? tb_decimal_to_double(bt->candles_tf[idx - 5].close)
            : tb_decimal_to_double(bt->candles[idx - 5].close);
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
    /* Simulate Fear & Greed based on recent price action (use TF candles) */
    int fg = 50;
    int use_tf = (bt->candles_tf && bt->n_candles_tf > 0);
    int idx = use_tf ? bt->current_tf_idx : bt->current_idx;
    if (idx > 20) {
        double prev = use_tf
            ? tb_decimal_to_double(bt->candles_tf[idx - 20].close)
            : tb_decimal_to_double(bt->candles[idx - 20].close);
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
        {"cancel_all_exchange", bt_lua_cancel_all},  /* alias for live compat */
        {"get_position",     bt_lua_get_position},
        {"get_mid_price",    bt_lua_get_mid_price},
        {"get_open_orders",  bt_lua_get_open_orders},
        {"get_account_value",bt_lua_get_account_value},
        {"get_daily_pnl",    bt_lua_get_daily_pnl},
        {"get_candles",      bt_lua_get_candles},
        {"get_indicators",   bt_lua_get_indicators},
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

    if (!isfinite(price) || price <= 0 || !isfinite(size) || size <= 0) {
        lua_pushnil(L);
        return 1;
    }

    tb_side_t side;
    if (strcmp(side_str, "BUY") == 0 || strcmp(side_str, "buy") == 0 ||
        strcmp(side_str, "B") == 0)
        side = TB_SIDE_BUY;
    else if (strcmp(side_str, "SELL") == 0 || strcmp(side_str, "sell") == 0 ||
             strcmp(side_str, "A") == 0 || strcmp(side_str, "S") == 0)
        side = TB_SIDE_SELL;
    else {
        lua_pushnil(L);
        return 1;
    }

    tb_tif_t tif = TB_TIF_GTC;
    bool reduce_only = false;
    if (lua_istable(L, 5)) {
        lua_getfield(L, 5, "tif");
        if (lua_isstring(L, -1)) {
            const char *t = lua_tostring(L, -1);
            if (strcmp(t, "ioc") == 0 || strcmp(t, "IOC") == 0) tif = TB_TIF_IOC;
            else if (strcmp(t, "alo") == 0 || strcmp(t, "ALO") == 0) tif = TB_TIF_ALO;
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

    /* Enforce max leverage (skip for reduce_only orders) */
    if (!reduce_only && bt->cfg.max_leverage > 0 && bt->balance > 0) {
        double existing_notional = fabs(bt->position_size) * bt->entry_price;
        double new_notional = price * size;
        double max_notional = bt->balance * bt->cfg.max_leverage;
        if (existing_notional + new_notional > max_notional * 1.05) {
            lua_pushnil(L);
            return 1;
        }
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
        .placed_at_idx = bt->current_idx,
    };
    if (slot >= bt->n_orders) bt->n_orders = slot + 1;

    lua_pushinteger(L, (lua_Integer)bt->orders[slot].oid);
    return 1;
}

static int bt_lua_place_trigger(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    /* bot.place_trigger(coin, side, price, size, trigger_px, tpsl)
     * Matches live API signature in strategy_api.c */
    const char *side_str = luaL_checkstring(L, 2);
    double price = luaL_checknumber(L, 3);
    double size = luaL_checknumber(L, 4);
    double trigger_px = luaL_optnumber(L, 5, price); /* trigger_px defaults to price */
    const char *tpsl = luaL_optstring(L, 6, "sl");

    if (!isfinite(price) || price <= 0 || !isfinite(size) || size <= 0 ||
        !isfinite(trigger_px) || trigger_px <= 0) {
        lua_pushnil(L);
        return 1;
    }

    tb_side_t side;
    if (strcmp(side_str, "BUY") == 0 || strcmp(side_str, "buy") == 0 ||
        strcmp(side_str, "B") == 0)
        side = TB_SIDE_BUY;
    else if (strcmp(side_str, "SELL") == 0 || strcmp(side_str, "sell") == 0 ||
             strcmp(side_str, "A") == 0 || strcmp(side_str, "S") == 0)
        side = TB_SIDE_SELL;
    else {
        lua_pushnil(L);
        return 1;
    }

    /* SL/TP triggers are always reduce_only */
    bool reduce_only = (strcmp(tpsl, "sl") == 0 || strcmp(tpsl, "tp") == 0);

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
        .placed_at_idx = bt->current_idx,
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
    double equity = bt->balance + bt->unrealized_pnl;
    lua_pushnumber(L, equity - bt->day_start_equity);
    return 1;
}

static int bt_lua_get_candles(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    /* bot.get_candles(coin, interval, limit) — returns aggregated TF candles */
    int limit = 100;
    if (lua_isnumber(L, 3)) limit = (int)lua_tointeger(L, 3);

    /* Use aggregated TF candles if available */
    if (bt->candles_tf && bt->n_candles_tf > 0) {
        int start = bt->current_tf_idx - limit + 1;
        if (start < 0) start = 0;

        lua_newtable(L);
        int n = 0;
        for (int i = start; i <= bt->current_tf_idx && i < bt->n_candles_tf; i++) {
            n++;
            lua_newtable(L);
            double o_val = tb_decimal_to_double(bt->candles_tf[i].open);
            double h_val = tb_decimal_to_double(bt->candles_tf[i].high);
            double l_val = tb_decimal_to_double(bt->candles_tf[i].low);
            double c_val = (i == bt->current_tf_idx)
                ? bt->current_mid
                : tb_decimal_to_double(bt->candles_tf[i].close);
            double v_val = tb_decimal_to_double(bt->candles_tf[i].volume);
            int64_t t_val = bt->candles_tf[i].time_open;

            lua_pushnumber(L, o_val); lua_setfield(L, -2, "open");
            lua_pushnumber(L, o_val); lua_setfield(L, -2, "o");
            lua_pushnumber(L, h_val); lua_setfield(L, -2, "high");
            lua_pushnumber(L, h_val); lua_setfield(L, -2, "h");
            lua_pushnumber(L, l_val); lua_setfield(L, -2, "low");
            lua_pushnumber(L, l_val); lua_setfield(L, -2, "l");
            lua_pushnumber(L, c_val); lua_setfield(L, -2, "close");
            lua_pushnumber(L, c_val); lua_setfield(L, -2, "c");
            lua_pushnumber(L, v_val); lua_setfield(L, -2, "volume");
            lua_pushnumber(L, v_val); lua_setfield(L, -2, "v");
            lua_pushinteger(L, t_val); lua_setfield(L, -2, "time");
            lua_pushinteger(L, t_val); lua_setfield(L, -2, "t");
            lua_rawseti(L, -2, n);
        }
        return 1;
    }

    /* Fallback: use raw candles (no TF aggregation) */
    int start = bt->current_idx - limit + 1;
    if (start < 0) start = 0;

    lua_newtable(L);
    int n = 0;
    for (int i = start; i <= bt->current_idx; i++) {
        n++;
        lua_newtable(L);
        double o_val = tb_decimal_to_double(bt->candles[i].open);
        double h_val = tb_decimal_to_double(bt->candles[i].high);
        double l_val = tb_decimal_to_double(bt->candles[i].low);
        double c_val = tb_decimal_to_double(bt->candles[i].close);
        double v_val = tb_decimal_to_double(bt->candles[i].volume);
        int64_t t_val = bt->candles[i].time_open;

        lua_pushnumber(L, o_val); lua_setfield(L, -2, "open");
        lua_pushnumber(L, o_val); lua_setfield(L, -2, "o");
        lua_pushnumber(L, h_val); lua_setfield(L, -2, "high");
        lua_pushnumber(L, h_val); lua_setfield(L, -2, "h");
        lua_pushnumber(L, l_val); lua_setfield(L, -2, "low");
        lua_pushnumber(L, l_val); lua_setfield(L, -2, "l");
        lua_pushnumber(L, c_val); lua_setfield(L, -2, "close");
        lua_pushnumber(L, c_val); lua_setfield(L, -2, "c");
        lua_pushnumber(L, v_val); lua_setfield(L, -2, "volume");
        lua_pushnumber(L, v_val); lua_setfield(L, -2, "v");
        lua_pushinteger(L, t_val); lua_setfield(L, -2, "time");
        lua_pushinteger(L, t_val); lua_setfield(L, -2, "t");
        lua_rawseti(L, -2, n);
    }
    return 1;
}

static int bt_lua_get_indicators(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    /* bot.get_indicators(coin, interval, limit) */
    int limit = 250;
    if (lua_isnumber(L, 3)) limit = (int)lua_tointeger(L, 3);

    /* Use aggregated TF candles if available */
    int use_tf = (bt->candles_tf && bt->n_candles_tf > 0);
    int cur_idx = use_tf ? bt->current_tf_idx : bt->current_idx;
    int start = cur_idx - limit + 1;
    if (start < 0) start = 0;
    int n = cur_idx - start + 1;
    if (n <= 0) { lua_pushnil(L); return 1; }

    tb_candle_input_t *inputs = malloc(sizeof(tb_candle_input_t) * (size_t)n);
    if (!inputs) { lua_pushnil(L); return 1; }

    if (use_tf) {
        for (int i = 0; i < n; i++) {
            int ci = start + i;
            inputs[i].open   = tb_decimal_to_double(bt->candles_tf[ci].open);
            inputs[i].high   = tb_decimal_to_double(bt->candles_tf[ci].high);
            inputs[i].low    = tb_decimal_to_double(bt->candles_tf[ci].low);
            inputs[i].close  = tb_decimal_to_double(bt->candles_tf[ci].close);
            inputs[i].volume = tb_decimal_to_double(bt->candles_tf[ci].volume);
            inputs[i].time_ms = bt->candles_tf[ci].time_open;
        }
        /* Inject current mid as live_price into last candle (like live) */
        if (n > 0) {
            double mid = bt->current_mid;
            inputs[n - 1].close = mid;
            if (mid > inputs[n - 1].high) inputs[n - 1].high = mid;
            if (mid < inputs[n - 1].low)  inputs[n - 1].low  = mid;
        }
    } else {
        for (int i = 0; i < n; i++) {
            int ci = start + i;
            inputs[i].open   = tb_decimal_to_double(bt->candles[ci].open);
            inputs[i].high   = tb_decimal_to_double(bt->candles[ci].high);
            inputs[i].low    = tb_decimal_to_double(bt->candles[ci].low);
            inputs[i].close  = tb_decimal_to_double(bt->candles[ci].close);
            inputs[i].volume = tb_decimal_to_double(bt->candles[ci].volume);
            inputs[i].time_ms = bt->candles[ci].time_open;
        }
    }

    tb_indicators_snapshot_t snap = tb_indicators_compute(inputs, n);
    free(inputs);

    lua_newtable(L);
    lua_pushboolean(L, snap.valid ? 1 : 0);  lua_setfield(L, -2, "valid");
    lua_pushnumber(L, snap.sma_20);   lua_setfield(L, -2, "sma_20");
    lua_pushnumber(L, snap.sma_50);   lua_setfield(L, -2, "sma_50");
    lua_pushnumber(L, snap.sma_200);  lua_setfield(L, -2, "sma_200");
    lua_pushnumber(L, snap.ema_12);   lua_setfield(L, -2, "ema_12");
    lua_pushnumber(L, snap.ema_26);   lua_setfield(L, -2, "ema_26");
    lua_pushnumber(L, snap.rsi_14);   lua_setfield(L, -2, "rsi_14");
    lua_pushnumber(L, snap.macd_line);      lua_setfield(L, -2, "macd_line");
    lua_pushnumber(L, snap.macd_line);      lua_setfield(L, -2, "macd");
    lua_pushnumber(L, snap.macd_signal);    lua_setfield(L, -2, "macd_signal");
    lua_pushnumber(L, snap.macd_histogram); lua_setfield(L, -2, "macd_histogram");
    lua_pushnumber(L, snap.bb_upper);  lua_setfield(L, -2, "bb_upper");
    lua_pushnumber(L, snap.bb_middle); lua_setfield(L, -2, "bb_middle");
    lua_pushnumber(L, snap.bb_lower);  lua_setfield(L, -2, "bb_lower");
    lua_pushnumber(L, snap.bb_width);  lua_setfield(L, -2, "bb_width");
    lua_pushnumber(L, snap.atr_14);    lua_setfield(L, -2, "atr_14");
    lua_pushnumber(L, snap.vwap);      lua_setfield(L, -2, "vwap");

    /* ADX */
    lua_pushnumber(L, snap.adx_14);    lua_setfield(L, -2, "adx");
    lua_pushnumber(L, snap.plus_di);   lua_setfield(L, -2, "plus_di");
    lua_pushnumber(L, snap.minus_di);  lua_setfield(L, -2, "minus_di");

    /* Keltner Channels */
    lua_pushnumber(L, snap.kc_upper);  lua_setfield(L, -2, "kc_upper");
    lua_pushnumber(L, snap.kc_middle); lua_setfield(L, -2, "kc_middle");
    lua_pushnumber(L, snap.kc_lower);  lua_setfield(L, -2, "kc_lower");

    /* Donchian Channels */
    lua_pushnumber(L, snap.dc_upper);  lua_setfield(L, -2, "dc_upper");
    lua_pushnumber(L, snap.dc_lower);  lua_setfield(L, -2, "dc_lower");
    lua_pushnumber(L, snap.dc_middle); lua_setfield(L, -2, "dc_middle");

    /* Stochastic RSI */
    lua_pushnumber(L, snap.stoch_rsi_k); lua_setfield(L, -2, "stoch_rsi_k");
    lua_pushnumber(L, snap.stoch_rsi_d); lua_setfield(L, -2, "stoch_rsi_d");

    /* CCI */
    lua_pushnumber(L, snap.cci_20);    lua_setfield(L, -2, "cci");

    /* Williams %R */
    lua_pushnumber(L, snap.williams_r);lua_setfield(L, -2, "williams_r");

    /* OBV */
    lua_pushnumber(L, snap.obv);       lua_setfield(L, -2, "obv");
    lua_pushnumber(L, snap.obv_sma);   lua_setfield(L, -2, "obv_sma");

    /* Ichimoku */
    lua_pushnumber(L, snap.ichi_tenkan);   lua_setfield(L, -2, "ichi_tenkan");
    lua_pushnumber(L, snap.ichi_kijun);    lua_setfield(L, -2, "ichi_kijun");
    lua_pushnumber(L, snap.ichi_senkou_a); lua_setfield(L, -2, "ichi_senkou_a");
    lua_pushnumber(L, snap.ichi_senkou_b); lua_setfield(L, -2, "ichi_senkou_b");
    lua_pushnumber(L, snap.ichi_chikou);   lua_setfield(L, -2, "ichi_chikou");

    /* CMF */
    lua_pushnumber(L, snap.cmf_20);        lua_setfield(L, -2, "cmf");

    /* MFI */
    lua_pushnumber(L, snap.mfi_14);        lua_setfield(L, -2, "mfi");

    /* Squeeze Momentum */
    lua_pushnumber(L, snap.squeeze_mom);   lua_setfield(L, -2, "squeeze_mom");
    lua_pushboolean(L, snap.squeeze_on);   lua_setfield(L, -2, "squeeze_on");

    /* ROC */
    lua_pushnumber(L, snap.roc_12);        lua_setfield(L, -2, "roc");

    /* Z-Score */
    lua_pushnumber(L, snap.zscore_20);     lua_setfield(L, -2, "zscore");

    /* FVG */
    lua_pushboolean(L, snap.fvg_bull);     lua_setfield(L, -2, "fvg_bull");
    lua_pushboolean(L, snap.fvg_bear);     lua_setfield(L, -2, "fvg_bear");
    lua_pushnumber(L, snap.fvg_size);      lua_setfield(L, -2, "fvg_size");

    /* Supertrend */
    lua_pushnumber(L, snap.supertrend);    lua_setfield(L, -2, "supertrend");
    lua_pushboolean(L, snap.supertrend_up);lua_setfield(L, -2, "supertrend_up");

    /* Parabolic SAR */
    lua_pushnumber(L, snap.psar);          lua_setfield(L, -2, "psar");
    lua_pushboolean(L, snap.psar_up);      lua_setfield(L, -2, "psar_up");

    /* Aliases for strategy compatibility */
    lua_pushnumber(L, snap.sma_20);    lua_setfield(L, -2, "sma");
    lua_pushnumber(L, snap.ema_12);    lua_setfield(L, -2, "ema");
    lua_pushnumber(L, snap.ema_12);    lua_setfield(L, -2, "ema_fast");
    lua_pushnumber(L, snap.ema_26);    lua_setfield(L, -2, "ema_slow");
    lua_pushnumber(L, snap.rsi_14);    lua_setfield(L, -2, "rsi");
    lua_pushnumber(L, snap.atr_14);    lua_setfield(L, -2, "atr");
    lua_pushnumber(L, snap.bb_middle); lua_setfield(L, -2, "bb_mid");

    lua_pushboolean(L, snap.above_sma_200);     lua_setfield(L, -2, "above_sma200");
    lua_pushboolean(L, snap.golden_cross);      lua_setfield(L, -2, "golden_cross");
    lua_pushboolean(L, snap.rsi_oversold);      lua_setfield(L, -2, "rsi_oversold");
    lua_pushboolean(L, snap.rsi_overbought);    lua_setfield(L, -2, "rsi_overbought");
    lua_pushboolean(L, snap.bb_squeeze);        lua_setfield(L, -2, "bb_squeeze");
    lua_pushboolean(L, snap.macd_bullish_cross);lua_setfield(L, -2, "macd_bullish");
    lua_pushboolean(L, snap.adx_trending);      lua_setfield(L, -2, "adx_trending");
    lua_pushboolean(L, snap.kc_squeeze);        lua_setfield(L, -2, "kc_squeeze");
    lua_pushboolean(L, snap.ichi_bullish);      lua_setfield(L, -2, "ichi_bullish");

    /* ── New derived indicators ── */
    lua_pushnumber(L, snap.atr_pct);           lua_setfield(L, -2, "atr_pct");
    lua_pushnumber(L, snap.atr_pct_rank);      lua_setfield(L, -2, "atr_pct_rank");
    lua_pushnumber(L, snap.range_pct_rank);    lua_setfield(L, -2, "range_pct_rank");
    lua_pushnumber(L, snap.vol_ratio);         lua_setfield(L, -2, "vol_ratio");
    lua_pushnumber(L, snap.ema12_dist_pct);    lua_setfield(L, -2, "ema12_dist_pct");
    lua_pushnumber(L, snap.sma20_dist_pct);    lua_setfield(L, -2, "sma20_dist_pct");
    lua_pushinteger(L, snap.consec_green);     lua_setfield(L, -2, "consec_green");
    lua_pushinteger(L, snap.consec_red);       lua_setfield(L, -2, "consec_red");
    lua_pushboolean(L, snap.bullish_engulf);   lua_setfield(L, -2, "bullish_engulf");
    lua_pushboolean(L, snap.bearish_engulf);   lua_setfield(L, -2, "bearish_engulf");
    lua_pushboolean(L, snap.shooting_star);    lua_setfield(L, -2, "shooting_star");
    lua_pushboolean(L, snap.hammer);           lua_setfield(L, -2, "hammer");
    lua_pushboolean(L, snap.doji);             lua_setfield(L, -2, "doji");
    lua_pushboolean(L, snap.macd_hist_incr);   lua_setfield(L, -2, "macd_accelerating");
    lua_pushboolean(L, snap.macd_hist_decr);   lua_setfield(L, -2, "macd_decelerating");
    lua_pushboolean(L, snap.di_bull);          lua_setfield(L, -2, "di_bull");
    lua_pushboolean(L, snap.di_bear);          lua_setfield(L, -2, "di_bear");
    lua_pushboolean(L, snap.rsi_bull_div);     lua_setfield(L, -2, "rsi_bull_divergence");
    lua_pushboolean(L, snap.rsi_bear_div);     lua_setfield(L, -2, "rsi_bear_divergence");

    return 1;
}

static int bt_lua_log(lua_State *L) {
    /* bot.log(level, msg) — output to stderr so diagnostic strategies can log */
    const char *level = lua_tostring(L, 1);
    const char *msg = lua_tostring(L, 2);
    if (level && msg) {
        fprintf(stderr, "bt-lua [%s]: %s\n", level, msg);
    }
    return 0;
}

static int bt_lua_time(lua_State *L) {
    tb_backtest_engine_t *bt = get_bt(L);
    /* Return seconds (double) to match live api_time() behavior.
       bt->current_time is in milliseconds (from candle time_ms). */
    lua_pushnumber(L, (double)bt->current_time / 1000.0);
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

    /* Multi-timeframe: default to 5m interval (no aggregation needed) */
    bt->tf_interval_ms = cfg->strategy_interval_ms > 0
        ? cfg->strategy_interval_ms : 300000LL;

    return bt;
}

static void bt_free_lua_state(lua_State *L) {
    if (!L) return;
    lua_close(L);
}

void tb_backtest_destroy(tb_backtest_engine_t *bt) {
    if (!bt) return;
    bt_free_lua_state(bt->L);
    bt->L = NULL;
    free(bt->candles);
    free(bt->candles_tf);
    free(bt);
}

int tb_backtest_load_candles(tb_backtest_engine_t *bt,
                             const tb_candle_t *candles, int n_candles) {
    if (n_candles <= 0) return -1;
    free(bt->candles);
    bt->candles = malloc(sizeof(tb_candle_t) * (size_t)n_candles);
    if (!bt->candles) return -1;
    memcpy(bt->candles, candles, sizeof(tb_candle_t) * n_candles);
    bt->n_candles = n_candles;
    return 0;
}

/* ── Aggregate a 5m candle into the strategy TF ───────────────────────── */
/* Returns true if a TF candle just completed (new boundary crossed). */
static bool aggregate_5m_into_tf(tb_backtest_engine_t *bt,
                                  const tb_candle_t *c5m) {
    int64_t time_ms = c5m->time_open;
    int64_t tf_ms = bt->tf_interval_ms;
    int64_t tf_start = (time_ms / tf_ms) * tf_ms;

    double o = tb_decimal_to_double(c5m->open);
    double h = tb_decimal_to_double(c5m->high);
    double l = tb_decimal_to_double(c5m->low);
    double c = tb_decimal_to_double(c5m->close);
    double v = tb_decimal_to_double(c5m->volume);

    bool tf_completed = false;

    if (bt->partial_count == 0) {
        /* First 5m candle ever — start partial */
        bt->partial_tf_start = tf_start;
        bt->partial_open = o;
        bt->partial_high = h;
        bt->partial_low = l;
        bt->partial_close = c;
        bt->partial_volume = v;
        bt->partial_count = 1;
        return false;
    }

    if (tf_start != bt->partial_tf_start) {
        /* New TF boundary — finalize the partial candle */
        if (bt->n_candles_tf >= bt->candles_tf_cap) {
            int new_cap = bt->candles_tf_cap == 0 ? 4096 : bt->candles_tf_cap * 2;
            tb_candle_t *new_buf = realloc(bt->candles_tf,
                                            sizeof(tb_candle_t) * (size_t)new_cap);
            if (!new_buf) return false;
            bt->candles_tf = new_buf;
            bt->candles_tf_cap = new_cap;
        }
        int idx = bt->n_candles_tf++;
        bt->candles_tf[idx].time_open = bt->partial_tf_start;
        bt->candles_tf[idx].time_close = bt->partial_tf_start + tf_ms;
        bt->candles_tf[idx].open   = tb_decimal_from_double(bt->partial_open, 8);
        bt->candles_tf[idx].high   = tb_decimal_from_double(bt->partial_high, 8);
        bt->candles_tf[idx].low    = tb_decimal_from_double(bt->partial_low, 8);
        bt->candles_tf[idx].close  = tb_decimal_from_double(bt->partial_close, 8);
        bt->candles_tf[idx].volume = tb_decimal_from_double(bt->partial_volume, 8);
        bt->current_tf_idx = idx;
        tf_completed = true;

        /* Start new partial */
        bt->partial_tf_start = tf_start;
        bt->partial_open = o;
        bt->partial_high = h;
        bt->partial_low = l;
        bt->partial_close = c;
        bt->partial_volume = v;
        bt->partial_count = 1;
    } else {
        /* Same TF — update partial */
        if (h > bt->partial_high) bt->partial_high = h;
        if (l < bt->partial_low) bt->partial_low = l;
        bt->partial_close = c;
        bt->partial_volume += v;
        bt->partial_count++;
    }

    return tf_completed;
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

    /* Determine if we need TF aggregation (strategy TF > 5m candle data) */
    bool do_aggregate = (bt->tf_interval_ms > 300000LL);

    /* Pre-allocate TF candle buffer */
    if (do_aggregate) {
        int est_tf_candles = (int)(bt->n_candles / (bt->tf_interval_ms / 300000LL)) + 10;
        bt->candles_tf_cap = est_tf_candles;
        bt->candles_tf = calloc((size_t)est_tf_candles, sizeof(tb_candle_t));
        bt->n_candles_tf = 0;
        bt->current_tf_idx = -1;
        bt->partial_count = 0;
    }

    /* Initialize Lua — no memory limit, backtests run fork-isolated */
    bt->L = luaL_newstate();
    if (!bt->L) return -1;
    luaL_openlibs(bt->L);
    setup_lua_sandbox(bt);

    /* Inject COIN global so generic strategies know which coin to trade */
    if (bt->cfg.coin[0] != '\0') {
        lua_pushstring(bt->L, bt->cfg.coin);
        lua_setglobal(bt->L, "COIN");
    }

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

    /* Main simulation loop — iterates over 5m candles */
    for (int i = 0; i < bt->n_candles; i++) {
        bt->current_idx = i;
        double close = tb_decimal_to_double(bt->candles[i].close);
        double high = tb_decimal_to_double(bt->candles[i].high);
        double low = tb_decimal_to_double(bt->candles[i].low);
        bt->current_mid = close;
        bt->current_time = bt->candles[i].time_open;

        /* 1. Aggregate this 5m candle into the strategy TF */
        bool tf_completed = false;
        if (do_aggregate) {
            tf_completed = aggregate_5m_into_tf(bt, &bt->candles[i]);
        }

        /* 2. Update unrealized P&L */
        if (bt->position_size != 0) {
            if (bt->position_size > 0)
                bt->unrealized_pnl = (close - bt->entry_price) * bt->position_size;
            else
                bt->unrealized_pnl = (bt->entry_price - close) * fabs(bt->position_size);
        } else {
            bt->unrealized_pnl = 0;
        }

        /* 3. Check order fills on 5m high/low (skip orders placed this tick) */
        check_order_fills(bt, high, low);

        /* 4. Re-update unrealized P&L after fills */
        if (bt->position_size != 0) {
            if (bt->position_size > 0)
                bt->unrealized_pnl = (close - bt->entry_price) * bt->position_size;
            else
                bt->unrealized_pnl = (bt->entry_price - close) * fabs(bt->position_size);
        } else {
            bt->unrealized_pnl = 0;
        }

        /* 5. Track daily equity reset (like live pos_tracker) */
        {
            int64_t day = bt->current_time / (86400LL * 1000);
            if (day != bt->current_day) {
                bt->current_day = day;
                bt->day_start_equity = bt->balance + bt->unrealized_pnl;
            }
        }

        /* Periodic GC to keep memory reasonable */
        if (i % 5000 == 0) {
            lua_gc(bt->L, LUA_GCCOLLECT, 0);
        }

        /* 6. on_tick: called only when a TF candle completes (or every candle if no aggregation) */
        bool should_tick = do_aggregate ? tf_completed : true;
        if (should_tick && (!do_aggregate || bt->current_tf_idx >= 0)) {
            /* When TF completes, use the TF candle's actual close (not the
               next-period 5m close) to avoid look-ahead bias. */
            double tick_price = close;
            if (do_aggregate && tf_completed && bt->current_tf_idx >= 0) {
                tick_price = tb_decimal_to_double(
                    bt->candles_tf[bt->current_tf_idx].close);
                bt->current_mid = tick_price;
            }
            lua_getglobal(bt->L, "on_tick");
            if (lua_isfunction(bt->L, -1)) {
                lua_pushstring(bt->L, bt->cfg.coin);
                lua_pushnumber(bt->L, tick_price);
                if (lua_pcall(bt->L, 2, 0, 0) != LUA_OK) {
                    tb_log_warn("backtest: on_tick error at candle %d: %s",
                                i, lua_tostring(bt->L, -1));
                    lua_pop(bt->L, 1);
                }
            } else {
                lua_pop(bt->L, 1);
            }

            /* Call on_timer at each TF tick (simulates periodic timer) */
            lua_getglobal(bt->L, "on_timer");
            if (lua_isfunction(bt->L, -1)) {
                if (lua_pcall(bt->L, 0, 0, 0) != LUA_OK) {
                    lua_pop(bt->L, 1);
                }
            } else {
                lua_pop(bt->L, 1);
            }

            /* 6b. Process any IOC orders placed by on_tick/on_timer immediately.
             * IOC = immediate-or-cancel, should not wait for next candle.
             * This fixes close_position() IOC orders being delayed 1 candle. */
            for (int j = 0; j < bt->n_orders; j++) {
                bt_order_t *o = &bt->orders[j];
                if (!o->active || o->tif != TB_TIF_IOC) continue;

                bool filled = false;
                double fill_price = bt->current_mid;

                if (o->side == TB_SIDE_BUY && bt->current_mid <= o->price)
                    filled = true;
                else if (o->side == TB_SIDE_SELL && bt->current_mid >= o->price)
                    filled = true;

                if (filled) {
                    if (o->reduce_only) {
                        if (bt->position_size == 0) { o->active = false; continue; }
                        if (o->side == TB_SIDE_BUY && bt->position_size > 0) {
                            o->active = false; continue;
                        }
                        if (o->side == TB_SIDE_SELL && bt->position_size < 0) {
                            o->active = false; continue;
                        }
                    }
                    o->active = false;  /* Deactivate BEFORE execute_fill to prevent slot reuse corruption */
                    execute_fill(bt, o->side, fill_price, o->size, true, o->oid);
                } else {
                    o->active = false; /* IOC cancelled — didn't fill */
                }
            }
        }

        /* 7. Track equity & drawdown */
        double equity = bt->balance + bt->unrealized_pnl;
        if (equity > peak_equity) peak_equity = equity;
        double dd = peak_equity - equity;
        if (dd > max_dd) max_dd = dd;
        double dd_pct = peak_equity > 0 ? dd / peak_equity * 100.0 : 0;
        if (dd_pct > max_dd_pct) max_dd_pct = dd_pct;

        /* Record equity curve (daily) */
        int64_t day = bt->current_time / (86400LL * 1000);
        if (day != last_equity_day && out->n_equity_points < 2000) {
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

    /* Trade stats — from running accumulators (not capped by trade log) */
    out->total_trades    = bt->stat_total;
    out->winning_trades  = bt->stat_wins;
    out->losing_trades   = bt->stat_losses;
    out->max_win         = bt->stat_max_win;
    out->max_loss        = bt->stat_max_loss;

    double gross_profit  = bt->stat_gross_profit;
    double gross_loss    = bt->stat_gross_loss;
    int    n_returns     = bt->stat_total;

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

    /* Sharpe & Sortino (annualized) — from Welford online accumulators */
    if (n_returns > 1 && out->n_days > 0) {
        double mean_ret = bt->stat_sum_ret / n_returns;
        double std_dev = sqrt(bt->stat_sum_ret_m2 / (n_returns - 1));
        double ann_factor = sqrt(365.0 / out->n_days * n_returns);
        if (std_dev > 0)
            out->sharpe_ratio = mean_ret / std_dev * ann_factor;

        double downside_dev = sqrt(bt->stat_sum_neg_sq / n_returns);
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

void tb_backtest_result_cleanup(tb_backtest_result_t *r) {
    if (r) {
        free(r->trades);
        r->trades = NULL;
        r->n_trade_log = 0;
        r->trade_cap = 0;
    }
}
