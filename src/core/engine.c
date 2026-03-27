#include "core/engine.h"
#include "core/logging.h"
#include "core/db.h"
#include "exchange/hl_signing.h"
#include "exchange/hl_rest.h"
#include "exchange/hl_ws.h"
#include "exchange/paper_exchange.h"
#include "exchange/position_tracker.h"
#include "exchange/order_manager.h"
#include "risk/risk_manager.h"
#include "strategy/lua_engine.h"
#include "data/data_manager.h"
#include "report/dashboard.h"
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <math.h>

/* ── Engine struct ─────────────────────────────────────────────────────────── */

struct tb_engine {
    tb_config_t              cfg;
    sqlite3                 *db;
    _Atomic bool             running;

    /* Exchange connectivity */
    hl_signer_t             *signer;        /* NULL if paper without key */
    hl_rest_t               *rest;          /* market data (info endpoints) */
    hl_ws_t                 *ws;            /* real-time prices */

    /* Paper trading */
    tb_paper_exchange_t     *paper;         /* global paper (NULL in live/mixed mode) */
    tb_paper_exchange_t     *paper_exchanges[8]; /* per-strategy paper (indexed by config) */

    /* Position & risk */
    tb_position_tracker_t    pos_tracker;
    tb_risk_mgr_t           *risk_mgr;

    /* Order management */
    tb_order_mgr_t          *order_mgr;

    /* Strategy */
    tb_lua_engine_t         *lua_engine;
    tb_lua_ctx_t             lua_ctx;

    /* Data feeds */
    tb_data_mgr_t           *data_mgr;

    /* Dashboard */
    tb_dashboard_t          *dashboard;

    /* Timer thread */
    pthread_t                timer_thread;
    _Atomic bool             timer_running;
    int64_t                  start_time;
};

/* ── Forward declarations for callbacks ────────────────────────────────────── */

static void engine_on_mids(const tb_mid_t *mids, int n_mids, void *userdata);
static void engine_on_ws_fill(const tb_fill_t *fills, int n_fills, void *userdata);
static void engine_on_ws_order_update(const tb_order_t *orders, int n_orders,
                                       void *userdata);
static void engine_on_paper_fill(const tb_fill_t *fill, void *userdata);
static void engine_on_order_fill(const tb_fill_t *fill, const char *strategy,
                                  void *userdata);
static void *engine_timer_thread(void *arg);

/* ── Create / Destroy ──────────────────────────────────────────────────────── */

tb_engine_t *tb_engine_create(const tb_config_t *cfg) {
    tb_engine_t *engine = calloc(1, sizeof(tb_engine_t));
    if (!engine) {
        tb_log_error("failed to allocate engine");
        return NULL;
    }

    memcpy(&engine->cfg, cfg, sizeof(tb_config_t));
    engine->running = false;

    /* Validate coin exclusivity: each coin must belong to exactly one strategy */
    {
        typedef struct { char coin[16]; char strategy[64]; } coin_owner_t;
        coin_owner_t owners[128];
        int n_owners = 0;

        for (int i = 0; i < cfg->n_active_strategies; i++) {
            for (int c = 0; c < cfg->n_strategy_coins[i]; c++) {
                const char *coin = cfg->strategy_coins[i][c];
                for (int k = 0; k < n_owners; k++) {
                    if (strcmp(owners[k].coin, coin) == 0) {
                        tb_log_error("engine: coin '%s' assigned to both '%s' and '%s' "
                                     "— each coin must belong to exactly one strategy",
                                     coin, owners[k].strategy, cfg->active_strategies[i]);
                        free(engine);
                        return NULL;
                    }
                }
                if (n_owners < 128) {
                    snprintf(owners[n_owners].coin, 16, "%s", coin);
                    snprintf(owners[n_owners].strategy, 64, "%s", cfg->active_strategies[i]);
                    n_owners++;
                }
            }
        }
    }

    /* Open database */
    if (tb_db_open(&engine->db, cfg->db_path) != 0) {
        free(engine);
        return NULL;
    }

    tb_log_info("engine created");
    return engine;
}

/* Secure wipe — volatile function pointer prevents dead-store elimination */
static void engine_secure_wipe(volatile void *ptr, size_t len) {
    static void *(*const volatile memset_fn)(void *, int, size_t) = memset;
    (memset_fn)((void *)ptr, 0, len);
}

void tb_engine_destroy(tb_engine_t *engine) {
    if (!engine) return;

    if (engine->running) {
        tb_engine_stop(engine);
    }

    tb_db_close(engine->db);

    /* Wipe all secrets from config before freeing */
    engine_secure_wipe(engine->cfg.private_key_hex, sizeof(engine->cfg.private_key_hex));
    engine_secure_wipe(engine->cfg.wallet_address, sizeof(engine->cfg.wallet_address));

    free(engine);
    tb_log_info("engine destroyed");
}

bool tb_engine_is_running(const tb_engine_t *engine) {
    return engine && engine->running;
}

/* ── Start: wire all subsystems ────────────────────────────────────────────── */

int tb_engine_start(tb_engine_t *engine) {
    if (engine->running) {
        tb_log_warn("engine already running");
        return 0;
    }

    /* Educational build: reject if paper mode was somehow disabled */
    if (!engine->cfg.paper_trading) {
        tb_log_error("engine: live trading disabled in educational build — aborting");
        return -1;
    }

    tb_log_info("engine starting...");
    tb_config_dump(&engine->cfg);
    engine->start_time = (int64_t)time(NULL);

    const tb_config_t *cfg = &engine->cfg;

    /* 1. Signer — skip if no private key (paper mode without key) */
    engine->signer = NULL;
    if (cfg->private_key_hex[0]) {
        tb_log_debug("signer: key length=%zu", strlen(cfg->private_key_hex));
        engine->signer = hl_signer_create(cfg->private_key_hex);
        if (!engine->signer) {
            tb_log_error("failed to create signer (key length=%zu, expected 64 hex or 0x+64)",
                         strlen(cfg->private_key_hex));
            goto fail;
        }
        /* Wipe private key from config — signer has its own copy */
        engine_secure_wipe(engine->cfg.private_key_hex,
                           sizeof(engine->cfg.private_key_hex));
        tb_log_info("signer created");
    } else {
        tb_log_info("no private key — signer skipped (paper mode)");
    }

    /* 2. REST client — for market data (info endpoints don't need auth) */
    engine->rest = hl_rest_create(cfg->rest_url, engine->signer,
                                   !cfg->is_testnet);
    if (!engine->rest) {
        tb_log_error("failed to create REST client");
        goto fail;
    }
    tb_log_info("REST client created (%s)", cfg->rest_url);

    /* 3. WebSocket — real-time mid prices */
    hl_ws_callbacks_t ws_cbs = {
        .on_mids         = engine_on_mids,
        .on_book         = NULL,
        .on_candle       = NULL,
        .on_order_update = engine_on_ws_order_update,
        .on_fill         = engine_on_ws_fill,
        .userdata        = engine,
    };
    engine->ws = hl_ws_create(cfg->ws_url, &ws_cbs);
    if (!engine->ws) {
        tb_log_error("failed to create WebSocket client");
        goto fail;
    }

    if (hl_ws_connect(engine->ws) != 0) {
        tb_log_error("failed to connect WebSocket");
        goto fail;
    }

    if (hl_ws_subscribe_all_mids(engine->ws) != 0) {
        tb_log_error("failed to subscribe to allMids");
        goto fail;
    }
    tb_log_info("WebSocket connected and subscribed to allMids");

    /* 4. Paper exchange(s) — global or per-strategy */
    engine->paper = NULL;
    memset(engine->paper_exchanges, 0, sizeof(engine->paper_exchanges));

    if (cfg->paper_trading) {
        /* Global paper mode: all strategies share one paper exchange */
        double initial_balance = cfg->paper_initial_balance > 0
            ? cfg->paper_initial_balance
            : 100.0;
        tb_log_info("paper: starting with global bankroll $%.2f", initial_balance);

        tb_paper_config_t paper_cfg = {
            .initial_balance = initial_balance,
            .maker_fee_rate  = 0.0002,
            .taker_fee_rate  = 0.0005,
        };
        engine->paper = tb_paper_create(&paper_cfg);
        if (!engine->paper) {
            tb_log_error("failed to create paper exchange");
            goto fail;
        }
        tb_paper_set_fill_cb(engine->paper, engine_on_paper_fill, engine);
        tb_log_info("paper exchange created (balance=%.2f USDC)",
                    paper_cfg.initial_balance);
    } else {
        /* Check per-strategy paper mode overrides */
        for (int i = 0; i < cfg->n_active_strategies; i++) {
            if (!cfg->strategy_paper_set[i] || !cfg->strategy_paper_mode[i])
                continue;

            double balance = cfg->strategy_paper_balance[i] > 0
                ? cfg->strategy_paper_balance[i]
                : cfg->paper_initial_balance > 0
                    ? cfg->paper_initial_balance
                    : 100.0;

            tb_paper_config_t paper_cfg = {
                .initial_balance = balance,
                .maker_fee_rate  = 0.0002,
                .taker_fee_rate  = 0.0005,
            };
            engine->paper_exchanges[i] = tb_paper_create(&paper_cfg);
            if (!engine->paper_exchanges[i]) {
                tb_log_error("failed to create paper exchange for strategy %s",
                             cfg->active_strategies[i]);
                goto fail;
            }
            tb_paper_set_fill_cb(engine->paper_exchanges[i],
                                  engine_on_paper_fill, engine);
            tb_log_info("paper exchange created for strategy '%s' (balance=%.2f USDC)",
                        cfg->active_strategies[i], balance);
        }
    }

    /* 5. Position tracker */
    if (tb_pos_tracker_init(&engine->pos_tracker) != 0) {
        tb_log_error("failed to init position tracker");
        goto fail;
    }
    tb_pos_tracker_load_cumulative(&engine->pos_tracker, engine->db);
    tb_log_info("position tracker initialized");

    /* 6. Risk manager */
    engine->risk_mgr = tb_risk_mgr_create(cfg);
    if (!engine->risk_mgr) {
        tb_log_error("failed to create risk manager");
        goto fail;
    }
    tb_risk_set_position_tracker(engine->risk_mgr, &engine->pos_tracker);
    tb_log_info("risk manager created");

    /* 7. Order manager */
    engine->order_mgr = tb_order_mgr_create(engine->rest, engine->ws,
                                              engine->risk_mgr,
                                              &engine->pos_tracker,
                                              engine->db);
    if (!engine->order_mgr) {
        tb_log_error("failed to create order manager");
        goto fail;
    }

    if (engine->paper) {
        tb_order_mgr_set_paper(engine->order_mgr, engine->paper);
    }

    /* Register per-strategy paper exchanges in order manager */
    for (int i = 0; i < cfg->n_active_strategies; i++) {
        if (!engine->paper_exchanges[i]) continue;

        /* Register each coin slot for this strategy */
        for (int c = 0; c < cfg->n_strategy_coins[i]; c++) {
            char coin_lower[16];
            snprintf(coin_lower, sizeof(coin_lower), "%s", cfg->strategy_coins[i][c]);
            for (char *p = coin_lower; *p; p++) {
                if (*p >= 'A' && *p <= 'Z') *p += 32;
            }
            char base_name[64];
            char *dot = strrchr(cfg->active_strategies[i], '.');
            if (dot) {
                size_t len = (size_t)(dot - cfg->active_strategies[i]);
                if (len >= sizeof(base_name)) len = sizeof(base_name) - 1;
                memcpy(base_name, cfg->active_strategies[i], len);
                base_name[len] = '\0';
            } else {
                snprintf(base_name, sizeof(base_name), "%s", cfg->active_strategies[i]);
            }
            char slot_name[64];
            snprintf(slot_name, sizeof(slot_name), "%s_%s", base_name, coin_lower);
            tb_order_mgr_set_strategy_paper(engine->order_mgr, slot_name,
                                             engine->paper_exchanges[i]);
        }
        /* Fallback: if no coins, register by filename without extension */
        if (cfg->n_strategy_coins[i] == 0) {
            char base_name[64];
            char *dot = strrchr(cfg->active_strategies[i], '.');
            if (dot) {
                size_t len = (size_t)(dot - cfg->active_strategies[i]);
                if (len >= sizeof(base_name)) len = sizeof(base_name) - 1;
                memcpy(base_name, cfg->active_strategies[i], len);
                base_name[len] = '\0';
            } else {
                snprintf(base_name, sizeof(base_name), "%s", cfg->active_strategies[i]);
            }
            tb_order_mgr_set_strategy_paper(engine->order_mgr, base_name,
                                             engine->paper_exchanges[i]);
        }
    }

    /* Set fill callback on order manager */
    tb_order_mgr_set_fill_callback(engine->order_mgr, engine_on_order_fill,
                                    engine);

    if (tb_order_mgr_start(engine->order_mgr, cfg->wallet_address) != 0) {
        tb_log_error("failed to start order manager");
        goto fail;
    }

    /* 8. Lua strategy engine */
    engine->lua_engine = tb_lua_engine_create(cfg);
    if (!engine->lua_engine) {
        tb_log_error("failed to create Lua engine");
        goto fail;
    }

    /* Set Lua context BEFORE loading strategies (load_strategy_file needs it) */
    engine->lua_ctx = (tb_lua_ctx_t){
        .order_mgr     = engine->order_mgr,
        .risk_mgr      = engine->risk_mgr,
        .rest           = engine->rest,
        .pos_tracker    = &engine->pos_tracker,
        .data_mgr       = NULL, /* set after data_mgr creation */
        .config          = cfg,
        .strategy_name   = NULL,
    };
    tb_lua_engine_set_context(engine->lua_engine, &engine->lua_ctx);

    int n_loaded = tb_lua_engine_load_strategies(engine->lua_engine);
    if (n_loaded < 0) {
        tb_log_error("failed to load strategies");
        goto fail;
    } else if (n_loaded == 0) {
        tb_log_warn("no strategies loaded");
    }

    /* Wire per-strategy paper exchanges to Lua slots */
    for (int i = 0; i < cfg->n_active_strategies; i++) {
        if (!engine->paper_exchanges[i]) continue;

        for (int c = 0; c < cfg->n_strategy_coins[i]; c++) {
            char coin_lower[16];
            snprintf(coin_lower, sizeof(coin_lower), "%s", cfg->strategy_coins[i][c]);
            for (char *p = coin_lower; *p; p++) {
                if (*p >= 'A' && *p <= 'Z') *p += 32;
            }
            char base_name[64];
            char *dot = strrchr(cfg->active_strategies[i], '.');
            if (dot) {
                size_t len = (size_t)(dot - cfg->active_strategies[i]);
                if (len >= sizeof(base_name)) len = sizeof(base_name) - 1;
                memcpy(base_name, cfg->active_strategies[i], len);
                base_name[len] = '\0';
            } else {
                snprintf(base_name, sizeof(base_name), "%s", cfg->active_strategies[i]);
            }
            char slot_name[64];
            snprintf(slot_name, sizeof(slot_name), "%s_%s", base_name, coin_lower);
            tb_lua_engine_set_strategy_paper(engine->lua_engine, slot_name,
                                              engine->paper_exchanges[i]);
        }
    }

    tb_lua_engine_on_init(engine->lua_engine);
    tb_log_info("Lua engine started, strategies initialized");

    /* 9. Data manager */
    engine->data_mgr = tb_data_mgr_create(cfg);
    if (engine->data_mgr) {
        engine->lua_ctx.data_mgr = engine->data_mgr;
        if (tb_data_mgr_start(engine->data_mgr) != 0) {
            tb_log_warn("failed to start data manager");
        } else {
            tb_log_info("data manager started");
        }
    } else {
        tb_log_warn("data manager creation failed (continuing without)");
    }

    /* 10. Dashboard */
    engine->dashboard = tb_dashboard_create(500);
    if (engine->dashboard) {
        if (tb_dashboard_start(engine->dashboard) != 0) {
            tb_log_warn("failed to start dashboard");
        } else {
            tb_log_info("dashboard started");
        }
    } else {
        tb_log_warn("dashboard creation failed");
    }

    /* 11. Timer thread — periodic tasks */
    engine->running = true;
    engine->timer_running = true;
    if (pthread_create(&engine->timer_thread, NULL, engine_timer_thread,
                       engine) != 0) {
        tb_log_warn("failed to start timer thread");
        engine->timer_running = false;
    }

    tb_log_info("engine started (mode: PAPER — educational)");
    return 0;

fail:
    tb_log_error("engine start failed, cleaning up...");
    tb_engine_stop(engine);
    return -1;
}

/* ── Stop: reverse shutdown ────────────────────────────────────────────────── */

void tb_engine_stop(tb_engine_t *engine) {
    if (!engine) return;
    tb_log_info("engine stopping...");
    engine->running = false;

    /* Timer thread */
    if (engine->timer_running) {
        engine->timer_running = false;
        pthread_join(engine->timer_thread, NULL);
        tb_log_info("timer thread stopped");
    }

    /* WebSocket FIRST — stops all WS callbacks before destroying subsystems */
    if (engine->ws) {
        hl_ws_disconnect(engine->ws);
        hl_ws_destroy(engine->ws);
        engine->ws = NULL;
        tb_log_info("WebSocket disconnected");
    }

    /* Dashboard */
    if (engine->dashboard) {
        tb_dashboard_stop(engine->dashboard);
        tb_dashboard_destroy(engine->dashboard);
        engine->dashboard = NULL;
        tb_log_info("dashboard stopped");
    }

    /* Lua shutdown */
    if (engine->lua_engine) {
        tb_lua_engine_on_shutdown(engine->lua_engine);
        tb_lua_engine_destroy(engine->lua_engine);
        engine->lua_engine = NULL;
        tb_log_info("Lua engine stopped");
    }

    /* Data manager */
    if (engine->data_mgr) {
        tb_data_mgr_stop(engine->data_mgr);
        tb_data_mgr_destroy(engine->data_mgr);
        engine->data_mgr = NULL;
        tb_log_info("data manager stopped");
    }

    /* Order manager */
    if (engine->order_mgr) {
        tb_order_mgr_destroy(engine->order_mgr);
        engine->order_mgr = NULL;
        tb_log_info("order manager stopped");
    }

    /* REST */
    if (engine->rest) {
        hl_rest_destroy(engine->rest);
        engine->rest = NULL;
    }

    /* Risk manager */
    if (engine->risk_mgr) {
        tb_risk_mgr_destroy(engine->risk_mgr);
        engine->risk_mgr = NULL;
    }

    /* Position tracker */
    tb_pos_tracker_destroy(&engine->pos_tracker);

    /* Paper exchange(s) */
    if (engine->paper) {
        tb_paper_destroy(engine->paper);
        engine->paper = NULL;
    }
    for (int p = 0; p < 8; p++) {
        if (engine->paper_exchanges[p]) {
            tb_paper_destroy(engine->paper_exchanges[p]);
            engine->paper_exchanges[p] = NULL;
        }
    }

    /* Signer */
    if (engine->signer) {
        hl_signer_destroy(engine->signer);
        engine->signer = NULL;
    }

    tb_log_info("engine stopped");
}

/* ── WebSocket callback: mid prices ────────────────────────────────────────── */

/* ── Price sanity filter (anti-MitM) ─────────────────────────────────────── */
#define PRICE_CACHE_MAX 64
#define PRICE_MAX_DEVIATION 0.50  /* reject >50% deviation from last known */

typedef struct {
    char   coin[16];
    double last_price;
    int    tick_count;    /* how many valid ticks received */
} price_sanity_entry_t;

static price_sanity_entry_t g_price_cache[PRICE_CACHE_MAX];
static int g_price_cache_n = 0;
static pthread_mutex_t g_price_lock = PTHREAD_MUTEX_INITIALIZER;

static price_sanity_entry_t *price_sanity_find(const char *coin) {
    for (int i = 0; i < g_price_cache_n; i++) {
        if (strcmp(g_price_cache[i].coin, coin) == 0)
            return &g_price_cache[i];
    }
    return NULL;
}

static bool price_sanity_check(const char *coin, double mid) {
    if (mid <= 0.0 || !isfinite(mid)) {
        tb_log_warn("price sanity: REJECTED %s mid=%.8f (non-positive/NaN)", coin, mid);
        return false;
    }

    pthread_mutex_lock(&g_price_lock);

    price_sanity_entry_t *e = price_sanity_find(coin);
    if (!e) {
        /* First price for this coin — accept but log */
        if (g_price_cache_n < PRICE_CACHE_MAX) {
            e = &g_price_cache[g_price_cache_n++];
            snprintf(e->coin, sizeof(e->coin), "%s", coin);
            e->last_price = mid;
            e->tick_count = 1;
        }
        pthread_mutex_unlock(&g_price_lock);
        return true;
    }

    double deviation = fabs(mid - e->last_price) / e->last_price;
    if (deviation > PRICE_MAX_DEVIATION) {
        pthread_mutex_unlock(&g_price_lock);
        tb_log_warn("price sanity: REJECTED %s mid=%.4f (%.1f%% deviation from %.4f)",
                     coin, mid, deviation * 100.0, e->last_price);
        return false;
    }

    e->last_price = mid;
    e->tick_count++;
    pthread_mutex_unlock(&g_price_lock);
    return true;
}

static void engine_on_mids(const tb_mid_t *mids, int n_mids, void *userdata) {
    tb_engine_t *engine = (tb_engine_t *)userdata;
    if (!engine || !engine->running) return;

    for (int i = 0; i < n_mids; i++) {
        double mid = tb_decimal_to_double(mids[i].mid);

        /* Price sanity check: reject >50% deviation from last known price */
        if (!price_sanity_check(mids[i].coin, mid)) {
            continue;  /* skip this poisoned price */
        }

        /* Feed to circuit breaker (tracks price velocity per coin) */
        if (engine->risk_mgr) {
            tb_risk_circuit_breaker_check(engine->risk_mgr, mids[i].coin, mid);
        }

        /* Feed to Lua strategies */
        if (engine->lua_engine) {
            tb_lua_engine_on_tick(engine->lua_engine, mids[i].coin, mid);
        }

        /* Feed to paper exchange(s) for order matching */
        if (engine->paper) {
            tb_paper_feed_mid(engine->paper, mids[i].coin, mid);
        }
        for (int p = 0; p < engine->cfg.n_active_strategies; p++) {
            if (engine->paper_exchanges[p])
                tb_paper_feed_mid(engine->paper_exchanges[p], mids[i].coin, mid);
        }
    }
}

/* ── WS fill / order update callbacks (live mode) ──────────────────────────── */

static void engine_on_ws_fill(const tb_fill_t *fills, int n_fills, void *userdata) {
    tb_engine_t *engine = (tb_engine_t *)userdata;
    if (!engine || !engine->running || !engine->order_mgr) return;
    tb_order_mgr_handle_ws_fills(engine->order_mgr, fills, n_fills);
}

static void engine_on_ws_order_update(const tb_order_t *orders, int n_orders,
                                       void *userdata) {
    tb_engine_t *engine = (tb_engine_t *)userdata;
    if (!engine || !engine->running || !engine->order_mgr) return;
    tb_order_mgr_handle_ws_orders(engine->order_mgr, orders, n_orders);
}

/* ── Paper exchange fill callback ──────────────────────────────────────────── */

static void engine_on_paper_fill(const tb_fill_t *fill, void *userdata) {
    tb_engine_t *engine = (tb_engine_t *)userdata;
    if (!engine || !engine->running) return;

    tb_log_info("PAPER FILL: %s %s %.4f @ %.2f",
                fill->side == TB_SIDE_BUY ? "BUY" : "SELL",
                fill->coin,
                tb_decimal_to_double(fill->sz),
                tb_decimal_to_double(fill->px));

    /* Route through order_manager: finds strategy via oid→strategy cache,
     * updates position tracker + risk, logs trade to DB, notifies Lua */
    if (engine->order_mgr) {
        tb_order_mgr_handle_ws_fills(engine->order_mgr, fill, 1);
    }
}

/* ── Order manager fill callback (live mode) ───────────────────────────────── */

static void engine_on_order_fill(const tb_fill_t *fill, const char *strategy,
                                  void *userdata) {
    tb_engine_t *engine = (tb_engine_t *)userdata;
    if (!engine || !engine->running) return;

    /* Notify Lua strategies of live fills */
    if (engine->lua_engine) {
        tb_lua_engine_on_fill(engine->lua_engine, fill, strategy);
    }
}

/* ── Dashboard update helper ───────────────────────────────────────────────── */

static void engine_update_dashboard(tb_engine_t *engine) {
    if (!engine->dashboard) return;

    tb_dashboard_data_t data;
    memset(&data, 0, sizeof(data));

    /* Account */
    if (engine->paper) {
        data.account_value = tb_paper_get_account_value(engine->paper);
        data.daily_pnl     = tb_paper_get_daily_pnl(engine->paper);
    } else {
        data.account_value = tb_pos_tracker_account_value(&engine->pos_tracker);
        data.daily_pnl     = tb_pos_tracker_daily_pnl(&engine->pos_tracker);
    }
    /* Update risk manager with current account value (for %-based limits) */
    if (engine->risk_mgr && data.account_value > 0) {
        tb_risk_update_account_value(engine->risk_mgr, data.account_value);
    }
    data.daily_fees      = tb_pos_tracker_daily_fees(&engine->pos_tracker);
    data.daily_trades    = tb_pos_tracker_daily_trades(&engine->pos_tracker);
    data.cumulative_pnl  = tb_pos_tracker_cumulative_pnl(&engine->pos_tracker);
    data.cumulative_fees = tb_pos_tracker_cumulative_fees(&engine->pos_tracker);

    /* Positions */
    tb_pos_tracker_get_all(&engine->pos_tracker,
                           data.positions, &data.n_positions);

    /* Open orders */
    if (engine->order_mgr) {
        tb_order_mgr_get_open_orders(engine->order_mgr, NULL,
                                      data.orders, &data.n_orders);
    }

    /* Market data */
    if (engine->data_mgr) {
        data.sentiment = tb_data_mgr_get_sentiment(engine->data_mgr);
        data.fear_greed = tb_data_mgr_get_fear_greed(engine->data_mgr);
    }

    /* Strategies */
    if (engine->lua_engine) {
        tb_lua_engine_get_strategies(engine->lua_engine,
                                      data.strategies, &data.n_strategies);
    }

    /* System */
    data.uptime_sec = (int64_t)time(NULL) - engine->start_time;
    data.paper_mode = engine->cfg.paper_trading;

    tb_dashboard_update(engine->dashboard, &data);
}

/* ── Timer thread ──────────────────────────────────────────────────────────── */

static void *engine_timer_thread(void *arg) {
    tb_engine_t *engine = (tb_engine_t *)arg;
    int tick = 0;
    char last_date[12] = {0};

    /* Initialize last_date to today */
    {
        time_t now = time(NULL);
        struct tm tm_buf;
        struct tm *tm = gmtime_r(&now, &tm_buf);
        if (tm) snprintf(last_date, sizeof(last_date), "%04d-%02d-%02d",
                         tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    }

    while (engine->timer_running) {
        usleep(1000000); /* 1 second */
        if (!engine->timer_running) break;
        tick++;

        /* Every 5 seconds: on_timer + hot reload check */
        if (tick % 5 == 0) {
            if (engine->lua_engine) {
                tb_lua_engine_on_timer(engine->lua_engine);
                tb_lua_engine_check_reload(engine->lua_engine);
            }
        }

        /* Every 60 seconds: check for UTC date rollover */
        if (tick % 60 == 0) {
            time_t now = time(NULL);
            struct tm tm_buf;
            struct tm *tm = gmtime_r(&now, &tm_buf);
            if (tm) {
                char today[12];
                snprintf(today, sizeof(today), "%04d-%02d-%02d",
                         tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
                if (strcmp(today, last_date) != 0) {
                    tb_log_info("daily rollover: %s -> %s", last_date, today);
                    snprintf(last_date, sizeof(last_date), "%s", today);
                    if (engine->risk_mgr)
                        tb_risk_reset_daily(engine->risk_mgr);
                    if (engine->paper)
                        tb_paper_reset_daily(engine->paper);
                    for (int p = 0; p < engine->cfg.n_active_strategies; p++) {
                        if (engine->paper_exchanges[p])
                            tb_paper_reset_daily(engine->paper_exchanges[p]);
                    }
                    tb_pos_tracker_reset_daily(&engine->pos_tracker);
                }
            }
        }

        /* Every second: dashboard update */
        engine_update_dashboard(engine);
    }

    return NULL;
}
