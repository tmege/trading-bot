#include "exchange/order_manager.h"
#include "risk/risk_manager.h"
#include "core/logging.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdatomic.h>

/* ── Order tracking ────────────────────────────────────────────────────────── */
#define MAX_OPEN_ORDERS 256

typedef struct {
    tb_order_t order;
    char       strategy[64];
} tracked_order_t;

struct tb_order_mgr {
    hl_rest_t              *rest;
    hl_ws_t                *ws;
    tb_risk_mgr_t          *risk;
    tb_position_tracker_t  *pos_tracker;
    sqlite3                *db;
    tb_paper_exchange_t    *paper;  /* non-NULL = paper trading mode */

    char                    user_addr[44];

    /* Local order state */
    tracked_order_t         orders[MAX_OPEN_ORDERS];
    int                     n_orders;
    pthread_rwlock_t        orders_lock;

    /* Fill callback */
    tb_on_fill_cb           fill_cb;
    void                   *fill_cb_userdata;

    /* Reconciliation thread */
    pthread_t               recon_thread;
    _Atomic bool            running;
    int                     recon_interval_sec;

    /* Prepared statements for trade logging */
    sqlite3_stmt           *stmt_insert_trade;
};

/* ── SQLite trade logging ──────────────────────────────────────────────────── */

static int prepare_statements(tb_order_mgr_t *mgr) {
    const char *sql =
        "INSERT INTO trades (timestamp_ms, coin, side, price, size, fee, pnl, "
        "oid, tid, strategy, cloid) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    int rc = sqlite3_prepare_v2(mgr->db, sql, -1, &mgr->stmt_insert_trade, NULL);
    if (rc != SQLITE_OK) {
        tb_log_error("failed to prepare trade insert: %s", sqlite3_errmsg(mgr->db));
        return -1;
    }
    return 0;
}

static void log_trade_to_db(tb_order_mgr_t *mgr, const tb_fill_t *fill,
                             const char *strategy) {
    if (!mgr->stmt_insert_trade) return;

    sqlite3_stmt *s = mgr->stmt_insert_trade;
    sqlite3_reset(s);

    char px_buf[64], sz_buf[64], fee_buf[64], pnl_buf[64];
    tb_decimal_to_str(fill->px, px_buf, sizeof(px_buf));
    tb_decimal_to_str(fill->sz, sz_buf, sizeof(sz_buf));
    tb_decimal_to_str(fill->fee, fee_buf, sizeof(fee_buf));
    tb_decimal_to_str(fill->closed_pnl, pnl_buf, sizeof(pnl_buf));

    sqlite3_bind_int64(s, 1, fill->time_ms);
    sqlite3_bind_text(s, 2, fill->coin, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, fill->side == TB_SIDE_BUY ? "buy" : "sell", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 4, px_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, sz_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 6, fee_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 7, pnl_buf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 8, (int64_t)fill->oid);
    sqlite3_bind_int64(s, 9, (int64_t)fill->tid);
    sqlite3_bind_text(s, 10, strategy ? strategy : "unknown", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 11, fill->hash, -1, SQLITE_STATIC);

    int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE) {
        tb_log_warn("failed to log trade: %s", sqlite3_errmsg(mgr->db));
    }
}

/* ── Find strategy name for an order (copies into caller buffer) ───────────── */
static bool find_strategy_for_oid(tb_order_mgr_t *mgr, uint64_t oid,
                                   char *out, size_t out_len) {
    pthread_rwlock_rdlock(&mgr->orders_lock);
    for (int i = 0; i < mgr->n_orders; i++) {
        if (mgr->orders[i].order.oid == oid) {
            snprintf(out, out_len, "%s", mgr->orders[i].strategy);
            pthread_rwlock_unlock(&mgr->orders_lock);
            return true;
        }
    }
    pthread_rwlock_unlock(&mgr->orders_lock);
    return false;
}

/* ── Remove order from tracking ────────────────────────────────────────────── */
static void remove_tracked_order(tb_order_mgr_t *mgr, uint64_t oid) {
    pthread_rwlock_wrlock(&mgr->orders_lock);
    for (int i = 0; i < mgr->n_orders; i++) {
        if (mgr->orders[i].order.oid == oid) {
            if (i < mgr->n_orders - 1) {
                memmove(&mgr->orders[i], &mgr->orders[i + 1],
                        sizeof(tracked_order_t) * (mgr->n_orders - i - 1));
            }
            mgr->n_orders--;
            break;
        }
    }
    pthread_rwlock_unlock(&mgr->orders_lock);
}

/* ── WebSocket callbacks (called from WS thread) ──────────────────────────── */

void tb_order_mgr_handle_ws_fills(tb_order_mgr_t *mgr,
                                   const tb_fill_t *fills, int n_fills) {

    for (int i = 0; i < n_fills; i++) {
        const tb_fill_t *fill = &fills[i];
        char strategy[64] = "unknown";
        find_strategy_for_oid(mgr, fill->oid, strategy, sizeof(strategy));

        /* Update position tracker */
        tb_pos_tracker_on_fill(mgr->pos_tracker, fill);

        /* Update risk manager */
        tb_risk_update_pnl(mgr->risk,
                           tb_decimal_to_double(fill->closed_pnl),
                           tb_decimal_to_double(fill->fee));

        /* Log to database */
        log_trade_to_db(mgr, fill, strategy);

        /* Notify strategy layer */
        if (mgr->fill_cb) {
            mgr->fill_cb(fill, strategy, mgr->fill_cb_userdata);
        }

        /* Don't remove here — WS order update (sz=0) or reconciliation
         * will handle removal.  Removing on every fill loses tracking
         * for partially-filled orders. */
    }
}

void tb_order_mgr_handle_ws_orders(tb_order_mgr_t *mgr,
                                    const tb_order_t *orders, int n_orders) {

    pthread_rwlock_wrlock(&mgr->orders_lock);
    for (int i = 0; i < n_orders; i++) {
        /* Find and update or remove */
        bool found = false;
        for (int j = 0; j < mgr->n_orders; j++) {
            if (mgr->orders[j].order.oid == orders[i].oid) {
                /* Update size (might be partially filled) */
                mgr->orders[j].order.sz = orders[i].sz;
                found = true;

                /* If size is zero, order is fully filled/cancelled */
                if (tb_decimal_is_zero(orders[i].sz)) {
                    if (j < mgr->n_orders - 1) {
                        memmove(&mgr->orders[j], &mgr->orders[j + 1],
                                sizeof(tracked_order_t) * (mgr->n_orders - j - 1));
                    }
                    mgr->n_orders--;
                }
                break;
            }
        }
        /* New order we didn't place? Ignore (could be from another client) */
        (void)found;
    }
    pthread_rwlock_unlock(&mgr->orders_lock);
}

/* ── Reconciliation thread ─────────────────────────────────────────────────── */

static void *recon_thread_func(void *arg) {
    tb_order_mgr_t *mgr = (tb_order_mgr_t *)arg;

    while (mgr->running) {
        sleep(mgr->recon_interval_sec);
        if (!mgr->running) break;

        tb_order_mgr_reconcile(mgr);
    }

    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

tb_order_mgr_t *tb_order_mgr_create(hl_rest_t *rest, hl_ws_t *ws,
                                     tb_risk_mgr_t *risk,
                                     tb_position_tracker_t *pos_tracker,
                                     sqlite3 *db) {
    tb_order_mgr_t *mgr = calloc(1, sizeof(tb_order_mgr_t));
    if (!mgr) return NULL;

    mgr->rest = rest;
    mgr->ws = ws;
    mgr->risk = risk;
    mgr->pos_tracker = pos_tracker;
    mgr->db = db;
    mgr->recon_interval_sec = 60;

    pthread_rwlock_init(&mgr->orders_lock, NULL);

    if (prepare_statements(mgr) != 0) {
        pthread_rwlock_destroy(&mgr->orders_lock);
        free(mgr);
        return NULL;
    }

    tb_log_info("order manager created");
    return mgr;
}

void tb_order_mgr_destroy(tb_order_mgr_t *mgr) {
    if (!mgr) return;
    tb_order_mgr_stop(mgr);
    if (mgr->stmt_insert_trade) sqlite3_finalize(mgr->stmt_insert_trade);
    pthread_rwlock_destroy(&mgr->orders_lock);
    free(mgr);
    tb_log_info("order manager destroyed");
}

int tb_order_mgr_start(tb_order_mgr_t *mgr, const char *user_addr) {
    snprintf(mgr->user_addr, sizeof(mgr->user_addr), "%s",
             user_addr ? user_addr : "paper");

    if (mgr->paper) {
        /* Paper mode: no WS subscriptions or REST reconciliation needed */
        mgr->running = true;
        tb_log_info("order manager started in PAPER mode");
        return 0;
    }

    /* Subscribe to WS updates */
    if (mgr->ws) {
        hl_ws_subscribe_order_updates(mgr->ws, user_addr);
        hl_ws_subscribe_user_fills(mgr->ws, user_addr);
    }

    /* Initial sync */
    tb_order_mgr_reconcile(mgr);

    /* Start reconciliation thread */
    mgr->running = true;
    if (pthread_create(&mgr->recon_thread, NULL, recon_thread_func, mgr) != 0) {
        mgr->running = false;
        tb_log_error("failed to start recon thread");
        return -1;
    }

    tb_log_info("order manager started for %s", user_addr);
    return 0;
}

void tb_order_mgr_stop(tb_order_mgr_t *mgr) {
    if (!mgr->running) return;
    mgr->running = false;
    if (!mgr->paper) {
        pthread_join(mgr->recon_thread, NULL);
    }
    tb_log_info("order manager stopped");
}

int tb_order_mgr_submit(tb_order_mgr_t *mgr, const tb_order_submit_t *submit,
                        uint64_t *out_oid) {
    /* Risk check — use coin name from order for position lookup */
    const char *coin = submit->order.coin;
    tb_position_t current_pos;
    tb_position_t *pos_ptr = NULL;
    if (coin[0] && tb_pos_tracker_get(mgr->pos_tracker, coin, &current_pos) == 0) {
        pos_ptr = &current_pos;
    }

    double account_val = tb_pos_tracker_account_value(mgr->pos_tracker);

    tb_risk_result_t risk_rc = tb_risk_check_order(
        mgr->risk, &submit->order, pos_ptr, account_val
    );

    if (risk_rc != TB_RISK_PASS) {
        tb_log_warn("order rejected by risk: %s (strategy=%s)",
                    tb_risk_result_str(risk_rc), submit->strategy_name);
        return -1;
    }

    /* Place order via paper exchange or REST */
    uint64_t oid = 0;
    int rc;
    if (mgr->paper) {
        rc = tb_paper_place_order(mgr->paper, &submit->order, &oid);
    } else {
        rc = hl_rest_place_order(mgr->rest, &submit->order,
                                  submit->order.grouping, &oid);
    }
    if (rc != 0) return -1;

    /* Track the order locally */
    pthread_rwlock_wrlock(&mgr->orders_lock);
    if (mgr->n_orders < MAX_OPEN_ORDERS) {
        tracked_order_t *t = &mgr->orders[mgr->n_orders];
        memset(t, 0, sizeof(*t));
        t->order.oid = oid;
        t->order.asset = submit->order.asset;
        snprintf(t->order.coin, sizeof(t->order.coin), "%s", coin);
        t->order.side = submit->order.side;
        t->order.limit_px = submit->order.price;
        t->order.sz = submit->order.size;
        t->order.tif = submit->order.tif;
        t->order.reduce_only = submit->order.reduce_only;
        snprintf(t->strategy, sizeof(t->strategy), "%s", submit->strategy_name);
        mgr->n_orders++;
    } else {
        tb_log_warn("order tracking full (%d), oid=%llu untracked",
                    MAX_OPEN_ORDERS, (unsigned long long)oid);
    }
    pthread_rwlock_unlock(&mgr->orders_lock);

    if (out_oid) *out_oid = oid;

    char px_str[64], sz_str[64];
    tb_decimal_to_str(submit->order.price, px_str, sizeof(px_str));
    tb_decimal_to_str(submit->order.size, sz_str, sizeof(sz_str));
    tb_log_info("order placed: oid=%llu %s %s @ %s sz=%s (strategy=%s)",
                (unsigned long long)oid,
                submit->order.side == TB_SIDE_BUY ? "BUY" : "SELL",
                coin,
                px_str, sz_str,
                submit->strategy_name);
    return 0;
}

int tb_order_mgr_cancel(tb_order_mgr_t *mgr, uint32_t asset, uint64_t oid) {
    int rc;
    if (mgr->paper) {
        rc = tb_paper_cancel_order(mgr->paper, asset, oid);
    } else {
        rc = hl_rest_cancel_order(mgr->rest, asset, oid);
    }
    if (rc == 0) {
        remove_tracked_order(mgr, oid);
    }
    return rc;
}

int tb_order_mgr_cancel_all_coin(tb_order_mgr_t *mgr, const char *coin) {
    /* Collect all oids for this coin */
    uint32_t assets[MAX_OPEN_ORDERS];
    uint64_t oids[MAX_OPEN_ORDERS];
    int n = 0;

    pthread_rwlock_rdlock(&mgr->orders_lock);
    for (int i = 0; i < mgr->n_orders; i++) {
        if (strcmp(mgr->orders[i].order.coin, coin) == 0 ||
            (coin[0] == '\0')) { /* empty = cancel all */
            assets[n] = mgr->orders[i].order.asset;
            oids[n] = mgr->orders[i].order.oid;
            n++;
        }
    }
    pthread_rwlock_unlock(&mgr->orders_lock);

    if (n == 0) return 0;

    int rc;
    if (mgr->paper) {
        rc = 0;
        for (int i = 0; i < n && rc == 0; i++)
            rc = tb_paper_cancel_order(mgr->paper, assets[i], oids[i]);
    } else {
        rc = hl_rest_cancel_orders(mgr->rest, assets, oids, n);
    }
    if (rc == 0) {
        for (int i = 0; i < n; i++) {
            remove_tracked_order(mgr, oids[i]);
        }
        tb_log_info("cancelled %d orders for %s", n, coin[0] ? coin : "ALL");
    }
    return rc;
}

int tb_order_mgr_get_open_orders(const tb_order_mgr_t *mgr, const char *coin,
                                  tb_order_t *out, int *count) {
    pthread_rwlock_rdlock((pthread_rwlock_t *)&mgr->orders_lock);
    int n = 0;
    for (int i = 0; i < mgr->n_orders; i++) {
        bool match = (coin == NULL || coin[0] == '\0' ||
                      strcmp(mgr->orders[i].order.coin, coin) == 0);
        if (match && out) {
            out[n] = mgr->orders[i].order;
        }
        if (match) n++;
    }
    *count = n;
    pthread_rwlock_unlock((pthread_rwlock_t *)&mgr->orders_lock);
    return 0;
}

int tb_order_mgr_open_order_count(const tb_order_mgr_t *mgr) {
    pthread_rwlock_rdlock((pthread_rwlock_t *)&mgr->orders_lock);
    int n = mgr->n_orders;
    pthread_rwlock_unlock((pthread_rwlock_t *)&mgr->orders_lock);
    return n;
}

void tb_order_mgr_set_paper(tb_order_mgr_t *mgr, tb_paper_exchange_t *paper) {
    mgr->paper = paper;
    tb_log_info("order manager: paper exchange %s",
                paper ? "enabled" : "disabled");
}

void tb_order_mgr_set_fill_callback(tb_order_mgr_t *mgr, tb_on_fill_cb cb,
                                     void *userdata) {
    mgr->fill_cb = cb;
    mgr->fill_cb_userdata = userdata;
}

int tb_order_mgr_reconcile(tb_order_mgr_t *mgr) {
    if (!mgr->user_addr[0]) return -1;
    if (mgr->paper) return 0; /* No REST reconciliation in paper mode */

    /* Fetch account state */
    tb_account_t account;
    if (hl_rest_get_account(mgr->rest, mgr->user_addr, &account) == 0) {
        tb_pos_tracker_sync(mgr->pos_tracker, &account);
    }

    /* Fetch open orders */
    tb_order_t *exchange_orders = malloc(sizeof(tb_order_t) * MAX_OPEN_ORDERS);
    if (!exchange_orders) return -1;
    int n_exchange = 0;
    if (hl_rest_get_open_orders(mgr->rest, mgr->user_addr,
                                 exchange_orders, &n_exchange,
                                 MAX_OPEN_ORDERS) == 0) {
        pthread_rwlock_wrlock(&mgr->orders_lock);

        /* Remove local orders not on exchange */
        for (int i = mgr->n_orders - 1; i >= 0; i--) {
            bool found = false;
            for (int j = 0; j < n_exchange; j++) {
                if (mgr->orders[i].order.oid == exchange_orders[j].oid) {
                    found = true;
                    /* Update size in case of partial fill */
                    mgr->orders[i].order.sz = exchange_orders[j].sz;
                    break;
                }
            }
            if (!found) {
                /* Order no longer on exchange — remove */
                if (i < mgr->n_orders - 1) {
                    memmove(&mgr->orders[i], &mgr->orders[i + 1],
                            sizeof(tracked_order_t) * (mgr->n_orders - i - 1));
                }
                mgr->n_orders--;
            }
        }

        int local_count = mgr->n_orders;
        pthread_rwlock_unlock(&mgr->orders_lock);

        tb_log_debug("reconciled: %d local orders, %d on exchange",
                     local_count, n_exchange);
    }

    /* Check emergency close */
    if (tb_risk_should_emergency_close(mgr->risk)) {
        tb_log_warn("EMERGENCY: daily loss limit hit, closing all positions");
        tb_order_mgr_cancel_all_coin(mgr, "");

        /* Close all open positions with market orders */
        tb_position_t positions[TB_MAX_POSITIONS];
        int n_pos = TB_MAX_POSITIONS;
        tb_pos_tracker_get_all(mgr->pos_tracker, positions, &n_pos);
        for (int p = 0; p < n_pos; p++) {
            double sz = tb_decimal_to_double(positions[p].size);
            if (fabs(sz) < 1e-12) continue;
            tb_order_submit_t close_order;
            memset(&close_order, 0, sizeof(close_order));
            snprintf(close_order.strategy_name, sizeof(close_order.strategy_name),
                     "emergency_close");
            close_order.order.asset = positions[p].asset;
            snprintf(close_order.order.coin, sizeof(close_order.order.coin),
                     "%s", positions[p].coin);
            close_order.order.side = sz > 0 ? TB_SIDE_SELL : TB_SIDE_BUY;
            close_order.order.size = tb_decimal_abs(positions[p].size);
            /* Use entry price ± 5% slippage for IOC.  Wide enough to fill
             * in volatile conditions, tight enough to avoid catastrophic fills. */
            double entry = tb_decimal_to_double(positions[p].entry_px);
            if (entry < 1e-9) entry = 1.0; /* fallback if no entry price */
            close_order.order.price = (sz > 0)
                ? tb_decimal_from_double(entry * 0.95, 6)
                : tb_decimal_from_double(entry * 1.05, 6);
            close_order.order.type = TB_ORDER_LIMIT;
            close_order.order.tif = TB_TIF_IOC;
            close_order.order.reduce_only = true;
            uint64_t close_oid = 0;
            if (mgr->paper) {
                tb_paper_place_order(mgr->paper, &close_order.order, &close_oid);
            } else {
                hl_rest_place_order(mgr->rest, &close_order.order,
                                    TB_GROUP_NA, &close_oid);
            }
            tb_log_warn("EMERGENCY: closing %s position sz=%.4f",
                        positions[p].coin, sz);
        }
    }

    free(exchange_orders);
    return 0;
}
