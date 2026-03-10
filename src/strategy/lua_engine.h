#ifndef TB_LUA_ENGINE_H
#define TB_LUA_ENGINE_H

#include "core/types.h"
#include "core/config.h"
#include "exchange/position_tracker.h"
#include <stdbool.h>
#include <stdint.h>

/* Forward declarations for opaque types */
typedef struct tb_order_mgr tb_order_mgr_t;
typedef struct tb_risk_mgr tb_risk_mgr_t;
typedef struct hl_rest hl_rest_t;
typedef struct tb_data_mgr tb_data_mgr_t;
typedef struct tb_lua_engine tb_lua_engine_t;

/* ── Strategy info ──────────────────────────────────────────────────────── */
#define TB_MAX_STRATEGIES 32

typedef struct {
    char    name[64];
    char    path[512];
    bool    loaded;
    bool    enabled;
    int64_t last_mtime;
} tb_strategy_info_t;

/* ── Context passed to all bot.* API calls ──────────────────────────────── */
typedef struct {
    tb_order_mgr_t          *order_mgr;
    tb_risk_mgr_t           *risk_mgr;
    hl_rest_t               *rest;
    tb_position_tracker_t   *pos_tracker;
    tb_data_mgr_t           *data_mgr;
    const tb_config_t       *config;
    const char              *strategy_name; /* current strategy being called */
} tb_lua_ctx_t;

/* ── Engine lifecycle ───────────────────────────────────────────────────── */

tb_lua_engine_t *tb_lua_engine_create(const tb_config_t *cfg);
void             tb_lua_engine_destroy(tb_lua_engine_t *engine);

/* Set the context used for bot.* API functions */
void tb_lua_engine_set_context(tb_lua_engine_t *engine, tb_lua_ctx_t *ctx);

/* Load all .lua files from strategies_dir */
int tb_lua_engine_load_strategies(tb_lua_engine_t *engine);

/* Check file mtimes and hot-reload changed strategies */
int tb_lua_engine_check_reload(tb_lua_engine_t *engine);

/* ── Callbacks (dispatched to all loaded strategies) ────────────────────── */

/* Called once after loading */
void tb_lua_engine_on_init(tb_lua_engine_t *engine);

/* Called on mid price update (from WS allMids) */
void tb_lua_engine_on_tick(tb_lua_engine_t *engine, const char *coin, double mid_price);

/* Called when an order is filled */
void tb_lua_engine_on_fill(tb_lua_engine_t *engine, const tb_fill_t *fill,
                            const char *strategy_name);

/* Called on periodic timer (configurable interval per strategy) */
void tb_lua_engine_on_timer(tb_lua_engine_t *engine);

/* Called on L2 book update */
void tb_lua_engine_on_book(tb_lua_engine_t *engine, const tb_book_t *book);

/* Called when AI advisory produces adjustments */
void tb_lua_engine_on_advisory(tb_lua_engine_t *engine, const char *json_adjustments);

/* Called on graceful shutdown */
void tb_lua_engine_on_shutdown(tb_lua_engine_t *engine);

/* ── Query ──────────────────────────────────────────────────────────────── */

/* Get list of loaded strategies */
int tb_lua_engine_get_strategies(const tb_lua_engine_t *engine,
                                  tb_strategy_info_t *out, int *count);

/* Enable/disable a strategy by name */
int tb_lua_engine_set_enabled(tb_lua_engine_t *engine, const char *name, bool enabled);

#endif /* TB_LUA_ENGINE_H */
