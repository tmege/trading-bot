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
#include "data/ai_advisor.h"
#include "report/dashboard.h"
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>

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
    tb_paper_exchange_t     *paper;         /* NULL in live mode */

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

    /* AI advisory */
    tb_ai_advisor_t         *ai_advisor;

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

    /* Open database */
    if (tb_db_open(&engine->db, cfg->db_path) != 0) {
        free(engine);
        return NULL;
    }

    tb_log_info("engine created");
    return engine;
}

/* Secure wipe that won't be optimized away */
static void engine_secure_wipe(volatile void *ptr, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) *p++ = 0;
}

void tb_engine_destroy(tb_engine_t *engine) {
    if (!engine) return;

    if (engine->running) {
        tb_engine_stop(engine);
    }

    tb_db_close(engine->db);

    /* Wipe all secrets from config before freeing */
    engine_secure_wipe(engine->cfg.private_key_hex, sizeof(engine->cfg.private_key_hex));
    engine_secure_wipe(engine->cfg.claude_api_key, sizeof(engine->cfg.claude_api_key));
    engine_secure_wipe(engine->cfg.macro_api_key, sizeof(engine->cfg.macro_api_key));
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
        tb_log_warn("failed to subscribe to allMids");
    }
    tb_log_info("WebSocket connected and subscribed to allMids");

    /* 4. Paper exchange (if paper trading mode) */
    engine->paper = NULL;
    if (cfg->paper_trading) {
        double initial_balance = cfg->paper_initial_balance > 0
            ? cfg->paper_initial_balance
            : 100.0;
        tb_log_info("paper: starting with bankroll $%.2f", initial_balance);

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
    }

    /* 5. Position tracker */
    if (tb_pos_tracker_init(&engine->pos_tracker) != 0) {
        tb_log_error("failed to init position tracker");
        goto fail;
    }
    tb_log_info("position tracker initialized");

    /* 6. Risk manager */
    engine->risk_mgr = tb_risk_mgr_create(cfg);
    if (!engine->risk_mgr) {
        tb_log_error("failed to create risk manager");
        goto fail;
    }
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

    /* 10. AI advisor (optional — needs claude_api_key) */
    engine->ai_advisor = NULL;
    if (cfg->claude_api_key[0]) {
        engine->ai_advisor = tb_ai_advisor_create(cfg, engine->db);
        if (engine->ai_advisor) {
            tb_ai_advisor_set_lua_engine(engine->ai_advisor, engine->lua_engine);
            if (tb_ai_advisor_start(engine->ai_advisor) != 0) {
                tb_log_warn("failed to start AI advisor");
            } else {
                tb_log_info("AI advisor started (model=%s)", cfg->claude_model);
            }
        } else {
            tb_log_warn("AI advisor creation failed (continuing without)");
        }
    } else {
        tb_log_info("AI advisor disabled (no API key)");
    }

    /* 11. Dashboard */
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

    /* 12. Timer thread — periodic tasks */
    engine->running = true;
    engine->timer_running = true;
    if (pthread_create(&engine->timer_thread, NULL, engine_timer_thread,
                       engine) != 0) {
        tb_log_warn("failed to start timer thread");
        engine->timer_running = false;
    }

    tb_log_info("engine started (mode: %s)",
                cfg->paper_trading ? "PAPER" : "LIVE");
    return 0;

fail:
    tb_log_error("engine start failed, cleaning up...");
    tb_engine_stop(engine);
    return -1;
}

/* ── Stop: reverse shutdown ────────────────────────────────────────────────── */

void tb_engine_stop(tb_engine_t *engine) {
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

    /* AI advisor */
    if (engine->ai_advisor) {
        tb_ai_advisor_stop(engine->ai_advisor);
        tb_ai_advisor_destroy(engine->ai_advisor);
        engine->ai_advisor = NULL;
        tb_log_info("AI advisor stopped");
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

    /* Paper exchange */
    if (engine->paper) {
        tb_paper_destroy(engine->paper);
        engine->paper = NULL;
    }

    /* Signer */
    if (engine->signer) {
        hl_signer_destroy(engine->signer);
        engine->signer = NULL;
    }

    tb_log_info("engine stopped");
}

/* ── WebSocket callback: mid prices ────────────────────────────────────────── */

static void engine_on_mids(const tb_mid_t *mids, int n_mids, void *userdata) {
    tb_engine_t *engine = (tb_engine_t *)userdata;
    if (!engine->running) return;

    for (int i = 0; i < n_mids; i++) {
        double mid = tb_decimal_to_double(mids[i].mid);

        /* Feed to circuit breaker (tracks price velocity per coin) */
        if (engine->risk_mgr) {
            tb_risk_circuit_breaker_check(engine->risk_mgr, mids[i].coin, mid);
        }

        /* Feed to Lua strategies */
        if (engine->lua_engine) {
            tb_lua_engine_on_tick(engine->lua_engine, mids[i].coin, mid);
        }

        /* Feed to paper exchange for order matching */
        if (engine->paper) {
            tb_paper_feed_mid(engine->paper, mids[i].coin, mid);
        }
    }
}

/* ── WS fill / order update callbacks (live mode) ──────────────────────────── */

static void engine_on_ws_fill(const tb_fill_t *fills, int n_fills, void *userdata) {
    tb_engine_t *engine = (tb_engine_t *)userdata;
    if (!engine->running || !engine->order_mgr) return;
    tb_order_mgr_handle_ws_fills(engine->order_mgr, fills, n_fills);
}

static void engine_on_ws_order_update(const tb_order_t *orders, int n_orders,
                                       void *userdata) {
    tb_engine_t *engine = (tb_engine_t *)userdata;
    if (!engine->running || !engine->order_mgr) return;
    tb_order_mgr_handle_ws_orders(engine->order_mgr, orders, n_orders);
}

/* ── Paper exchange fill callback ──────────────────────────────────────────── */

static void engine_on_paper_fill(const tb_fill_t *fill, void *userdata) {
    tb_engine_t *engine = (tb_engine_t *)userdata;
    if (!engine->running) return;

    /* Update position tracker */
    tb_pos_tracker_on_fill(&engine->pos_tracker, fill);

    /* Update risk P&L */
    if (engine->risk_mgr) {
        tb_risk_update_pnl(engine->risk_mgr,
                           tb_decimal_to_double(fill->closed_pnl),
                           tb_decimal_to_double(fill->fee));
    }

    /* Notify Lua strategies */
    if (engine->lua_engine) {
        tb_lua_engine_on_fill(engine->lua_engine, fill, "paper");
    }

    tb_log_info("PAPER FILL: %s %s %.4f @ %.2f",
                fill->side == TB_SIDE_BUY ? "BUY" : "SELL",
                fill->coin,
                tb_decimal_to_double(fill->sz),
                tb_decimal_to_double(fill->px));
}

/* ── Order manager fill callback (live mode) ───────────────────────────────── */

static void engine_on_order_fill(const tb_fill_t *fill, const char *strategy,
                                  void *userdata) {
    tb_engine_t *engine = (tb_engine_t *)userdata;
    if (!engine->running) return;

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
    data.daily_fees   = tb_pos_tracker_daily_fees(&engine->pos_tracker);
    data.daily_trades = tb_pos_tracker_daily_trades(&engine->pos_tracker);

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
        data.macro     = tb_data_mgr_get_macro(engine->data_mgr);
        data.sentiment = tb_data_mgr_get_sentiment(engine->data_mgr);
        data.fear_greed = tb_data_mgr_get_fear_greed(engine->data_mgr);
    }

    /* Strategies */
    if (engine->lua_engine) {
        tb_lua_engine_get_strategies(engine->lua_engine,
                                      data.strategies, &data.n_strategies);
    }

    /* Advisory */
    if (engine->ai_advisor) {
        data.last_advisory_ms = tb_ai_advisor_last_call_time(engine->ai_advisor);
    }

    /* System */
    data.uptime_sec = (int64_t)time(NULL) - engine->start_time;
    data.paper_mode = engine->cfg.paper_trading;

    tb_dashboard_update(engine->dashboard, &data);
}

/* ── Update AI advisor context from current engine state ───────────────────── */

static void engine_update_advisory_context(tb_engine_t *engine) {
    if (!engine->ai_advisor) return;

    tb_advisory_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Account */
    if (engine->paper) {
        ctx.account_value = tb_paper_get_account_value(engine->paper);
        ctx.daily_pnl     = tb_paper_get_daily_pnl(engine->paper);
    } else {
        ctx.account_value = tb_pos_tracker_account_value(&engine->pos_tracker);
        ctx.daily_pnl     = tb_pos_tracker_daily_pnl(&engine->pos_tracker);
    }
    ctx.daily_fees  = tb_pos_tracker_daily_fees(&engine->pos_tracker);
    ctx.daily_trades = tb_pos_tracker_daily_trades(&engine->pos_tracker);

    /* Positions */
    tb_pos_tracker_get_all(&engine->pos_tracker,
                           ctx.positions, &ctx.n_positions);

    /* Macro / sentiment / fear & greed */
    if (engine->data_mgr) {
        ctx.macro      = tb_data_mgr_get_macro(engine->data_mgr);
        ctx.sentiment  = tb_data_mgr_get_sentiment(engine->data_mgr);
        ctx.fear_greed = tb_data_mgr_get_fear_greed(engine->data_mgr);
    }

    /* Strategies — copy from tb_strategy_info_t to advisory struct */
    if (engine->lua_engine) {
        tb_strategy_info_t strats[8];
        int n_strats = 0;
        tb_lua_engine_get_strategies(engine->lua_engine, strats, &n_strats);
        ctx.n_strategies = n_strats > 8 ? 8 : n_strats;
        for (int i = 0; i < ctx.n_strategies; i++) {
            snprintf(ctx.strategies[i].name, sizeof(ctx.strategies[i].name),
                     "%s", strats[i].name);
            ctx.strategies[i].enabled = strats[i].enabled;
        }
    }

    tb_ai_advisor_update_context(engine->ai_advisor, &ctx);
}

/* ── Timer thread ──────────────────────────────────────────────────────────── */

static void *engine_timer_thread(void *arg) {
    tb_engine_t *engine = (tb_engine_t *)arg;
    int tick = 0;
    bool initial_advisory_done = false;
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

        /* Every 30 seconds: update AI advisor context */
        if (tick % 30 == 0 && engine->ai_advisor) {
            engine_update_advisory_context(engine);
        }

        /* First advisory call 15s after startup (let market data arrive) */
        if (!initial_advisory_done && tick == 15 && engine->ai_advisor) {
            tb_log_info("triggering initial AI advisory call...");
            engine_update_advisory_context(engine);
            tb_ai_advisor_call_now(engine->ai_advisor);
            initial_advisory_done = true;
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
                    tb_pos_tracker_reset_daily(&engine->pos_tracker);
                }
            }
        }

        /* Every second: dashboard update */
        engine_update_dashboard(engine);
    }

    return NULL;
}
