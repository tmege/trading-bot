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
#define OID_CACHE_SIZE  2048

typedef struct {
    tb_order_t order;
    char       strategy[64];
} tracked_order_t;

typedef struct {
    uint64_t oid;
    char     strategy[64];
} oid_cache_entry_t;

struct tb_order_mgr {
    hl_rest_t              *rest;
    hl_ws_t                *ws;
    tb_risk_mgr_t          *risk;
    tb_position_tracker_t  *pos_tracker;
    sqlite3                *db;
    tb_paper_exchange_t    *paper;  /* non-NULL = paper trading mode */

    char                    user_addr[44];

    /* Asset metadata (fetched from exchange) */
    tb_asset_meta_t         assets[TB_MAX_ASSETS];
    int                     n_assets;

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

    /* OID → strategy cache (survives reconciliation removal) */
    oid_cache_entry_t       oid_cache[OID_CACHE_SIZE];
    unsigned                oid_cache_idx;

    /* Prepared statements for trade logging */
    sqlite3_stmt           *stmt_insert_trade;

    /* Prepared statements for OID → strategy persistent map */
    sqlite3_stmt           *stmt_insert_oid_strat;
    sqlite3_stmt           *stmt_find_oid_strat;
    sqlite3_stmt           *stmt_cleanup_oid_strat;
};

/* ── SQLite trade logging ──────────────────────────────────────────────────── */

static int prepare_statements(tb_order_mgr_t *mgr) {
    const char *sql =
        "INSERT INTO trades (timestamp_ms, coin, side, price, size, fee, pnl, "
        "oid, tid, strategy, cloid) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(oid, tid) DO UPDATE SET "
        "  strategy = CASE WHEN trades.strategy = 'unknown' "
        "    THEN excluded.strategy ELSE trades.strategy END, "
        "  pnl = CASE WHEN trades.pnl = '0' OR trades.pnl = '' "
        "    THEN excluded.pnl ELSE trades.pnl END, "
        "  fee = CASE WHEN trades.fee = '0' OR trades.fee = '' "
        "    THEN excluded.fee ELSE trades.fee END";
    int rc = sqlite3_prepare_v2(mgr->db, sql, -1, &mgr->stmt_insert_trade, NULL);
    if (rc != SQLITE_OK) {
        tb_log_error("failed to prepare trade insert: %s", sqlite3_errmsg(mgr->db));
        return -1;
    }

    /* OID → strategy persistent map */
    const char *sql_ins_oid =
        "INSERT OR REPLACE INTO order_strategy_map (oid, strategy, coin, created_ms) "
        "VALUES (?, ?, ?, ?)";
    rc = sqlite3_prepare_v2(mgr->db, sql_ins_oid, -1, &mgr->stmt_insert_oid_strat, NULL);
    if (rc != SQLITE_OK) {
        tb_log_error("failed to prepare oid_strat insert: %s", sqlite3_errmsg(mgr->db));
        return -1;
    }

    const char *sql_find_oid =
        "SELECT strategy FROM order_strategy_map WHERE oid = ?";
    rc = sqlite3_prepare_v2(mgr->db, sql_find_oid, -1, &mgr->stmt_find_oid_strat, NULL);
    if (rc != SQLITE_OK) {
        tb_log_error("failed to prepare oid_strat find: %s", sqlite3_errmsg(mgr->db));
        return -1;
    }

    const char *sql_clean_oid =
        "DELETE FROM order_strategy_map WHERE created_ms < ?";
    rc = sqlite3_prepare_v2(mgr->db, sql_clean_oid, -1, &mgr->stmt_cleanup_oid_strat, NULL);
    if (rc != SQLITE_OK) {
        tb_log_error("failed to prepare oid_strat cleanup: %s", sqlite3_errmsg(mgr->db));
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
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        tb_log_warn("failed to log trade: %s (rc=%d)", sqlite3_errmsg(mgr->db), rc);
    }
}

/* ── Cache coin → strategy for trigger orders (fallback when OID mismatch) ── */
#define TRIGGER_COIN_CACHE_SIZE 32

typedef struct {
    char coin[16];
    char strategy[64];
    time_t cached_at;
} trigger_coin_entry_t;

static trigger_coin_entry_t g_trigger_coin_cache[TRIGGER_COIN_CACHE_SIZE];
static int g_trigger_coin_count = 0;

static void cache_trigger_coin(const char *coin, const char *strategy) {
    time_t now = time(NULL);
    /* Update existing entry for this coin */
    for (int i = 0; i < g_trigger_coin_count; i++) {
        if (strcmp(g_trigger_coin_cache[i].coin, coin) == 0) {
            snprintf(g_trigger_coin_cache[i].strategy,
                     sizeof(g_trigger_coin_cache[i].strategy), "%s", strategy);
            g_trigger_coin_cache[i].cached_at = now;
            return;
        }
    }
    /* Add new entry */
    if (g_trigger_coin_count < TRIGGER_COIN_CACHE_SIZE) {
        trigger_coin_entry_t *e = &g_trigger_coin_cache[g_trigger_coin_count++];
        snprintf(e->coin, sizeof(e->coin), "%s", coin);
        snprintf(e->strategy, sizeof(e->strategy), "%s", strategy);
        e->cached_at = now;
    }
}

static bool find_strategy_by_coin_trigger(const char *coin, char *out, size_t out_len) {
    time_t now = time(NULL);
    for (int i = 0; i < g_trigger_coin_count; i++) {
        if (strcmp(g_trigger_coin_cache[i].coin, coin) == 0 &&
            now - g_trigger_coin_cache[i].cached_at < 86400) {  /* 24h TTL */
            snprintf(out, out_len, "%s", g_trigger_coin_cache[i].strategy);
            return true;
        }
    }
    return false;
}

/* ── Cache OID → strategy (ring buffer + SQLite) ──────────────────────────── */
static void cache_oid_strategy(tb_order_mgr_t *mgr, uint64_t oid,
                                const char *strategy, const char *coin) {
    /* Ring buffer (single-writer, no lock needed) */
    unsigned idx = mgr->oid_cache_idx & (OID_CACHE_SIZE - 1);
    mgr->oid_cache[idx].oid = oid;
    snprintf(mgr->oid_cache[idx].strategy, sizeof(mgr->oid_cache[idx].strategy),
             "%s", strategy);
    mgr->oid_cache_idx++;

    /* 2. SQLite persistent table */
    if (mgr->stmt_insert_oid_strat) {
        sqlite3_stmt *s = mgr->stmt_insert_oid_strat;
        sqlite3_reset(s);
        sqlite3_bind_int64(s, 1, (int64_t)oid);
        sqlite3_bind_text(s, 2, strategy, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, coin, -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 4, (int64_t)time(NULL) * 1000);
        sqlite3_step(s);
    }
}

/* ── Find strategy name for an order (copies into caller buffer) ───────────── */
static bool find_strategy_for_oid(tb_order_mgr_t *mgr, uint64_t oid,
                                   char *out, size_t out_len) {
    /* 1. Check main tracking array (active orders) */
    pthread_rwlock_rdlock(&mgr->orders_lock);
    for (int i = 0; i < mgr->n_orders; i++) {
        if (mgr->orders[i].order.oid == oid) {
            snprintf(out, out_len, "%s", mgr->orders[i].strategy);
            pthread_rwlock_unlock(&mgr->orders_lock);
            return true;
        }
    }
    pthread_rwlock_unlock(&mgr->orders_lock);

    /* 2. Check ring buffer cache (survives reconciliation removal) */
    for (int i = 0; i < OID_CACHE_SIZE; i++) {
        if (mgr->oid_cache[i].oid == oid) {
            snprintf(out, out_len, "%s", mgr->oid_cache[i].strategy);
            return true;
        }
    }

    /* 3. Check SQLite persistent table (survives restart) */
    if (mgr->stmt_find_oid_strat) {
        sqlite3_reset(mgr->stmt_find_oid_strat);
        sqlite3_bind_int64(mgr->stmt_find_oid_strat, 1, (int64_t)oid);
        if (sqlite3_step(mgr->stmt_find_oid_strat) == SQLITE_ROW) {
            const char *s = (const char *)sqlite3_column_text(
                mgr->stmt_find_oid_strat, 0);
            if (s) {
                snprintf(out, out_len, "%s", s);
                return true;
            }
        }
    }

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

/* ── Fill deduplication (prevents WS snapshot redelivery on reconnect) ──────── */
#define SEEN_FILL_SIZE 128

static uint64_t  g_seen_tids[SEEN_FILL_SIZE];
static unsigned  g_seen_idx = 0;

static bool fill_already_seen(uint64_t tid) {
    if (tid == 0) return false;
    for (int i = 0; i < SEEN_FILL_SIZE; i++) {
        if (g_seen_tids[i] == tid) return true;
    }
    return false;
}

static void fill_mark_seen(uint64_t tid) {
    if (tid == 0) return;
    g_seen_tids[g_seen_idx & (SEEN_FILL_SIZE - 1)] = tid;
    g_seen_idx++;
}

/* ── WebSocket callbacks (called from WS thread) ──────────────────────────── */

void tb_order_mgr_handle_ws_fills(tb_order_mgr_t *mgr,
                                   const tb_fill_t *fills, int n_fills) {

    for (int i = 0; i < n_fills; i++) {
        const tb_fill_t *fill = &fills[i];

        /* Deduplicate by tid: skip fills already processed (WS snapshot redelivery) */
        if (fill_already_seen(fill->tid)) {
            tb_log_debug("fill dedup: skipping tid=%llu oid=%llu (already processed)",
                         (unsigned long long)fill->tid,
                         (unsigned long long)fill->oid);
            continue;
        }
        fill_mark_seen(fill->tid);

        tb_log_debug("fill: %s %s oid=%llu tid=%llu (%d/%d)",
                     fill->side == TB_SIDE_BUY ? "BUY" : "SELL",
                     fill->coin,
                     (unsigned long long)fill->oid,
                     (unsigned long long)fill->tid,
                     i + 1, n_fills);

        char strategy[64] = "unknown";
        find_strategy_for_oid(mgr, fill->oid, strategy, sizeof(strategy));

        /* Fallback: if OID lookup failed and fill has closed_pnl (exit fill),
         * try coin-based trigger cache. This handles the Hyperliquid trigger
         * OID mismatch: trigger placement OID ≠ triggered fill OID. */
        if (strcmp(strategy, "unknown") == 0 &&
            !tb_decimal_is_zero(fill->closed_pnl)) {
            if (find_strategy_by_coin_trigger(fill->coin, strategy, sizeof(strategy))) {
                tb_log_info("fill: OID %llu unknown, resolved %s via coin fallback (%s)",
                            (unsigned long long)fill->oid, strategy, fill->coin);
            }
        }

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

/* ── Asset resolution helpers ──────────────────────────────────────────────── */

/* Look up asset metadata by coin name. Returns NULL if not found. */
static const tb_asset_meta_t *find_asset(const tb_order_mgr_t *mgr,
                                          const char *coin) {
    for (int i = 0; i < mgr->n_assets; i++) {
        if (strcmp(mgr->assets[i].name, coin) == 0) {
            return &mgr->assets[i];
        }
    }
    return NULL;
}

/* Round price to max 5 significant figures (Hyperliquid requirement).
 * E.g. BTC 69474.9 → 69475, ETH 2017.57 → 2017.6, SOL 85.479 → 85.479 */
static tb_decimal_t round_price_sigfigs(double price, int max_sigfigs) {
    if (price <= 0.0) return tb_decimal_from_double(0, 0);

    /* Count integer digits */
    int int_digits = 0;
    double tmp = price;
    while (tmp >= 1.0) { tmp /= 10.0; int_digits++; }

    /* Max decimal places = max(max_sigfigs - int_digits, 0) */
    int max_decimals = max_sigfigs - int_digits;
    if (max_decimals < 0) max_decimals = 0;
    if (max_decimals > 8) max_decimals = 8;

    /* Round to that many decimal places */
    double factor = 1.0;
    for (int i = 0; i < max_decimals; i++) factor *= 10.0;
    double rounded = round(price * factor) / factor;

    return tb_decimal_from_double(rounded, (uint8_t)max_decimals);
}

/* Round size to sz_decimals from asset metadata */
static tb_decimal_t round_size(double size, int sz_decimals) {
    if (sz_decimals < 0) sz_decimals = 0;
    if (sz_decimals > 8) sz_decimals = 8;
    return tb_decimal_from_double(size, (uint8_t)sz_decimals);
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
    mgr->recon_interval_sec = 15;

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
    if (mgr->stmt_insert_trade)     sqlite3_finalize(mgr->stmt_insert_trade);
    if (mgr->stmt_insert_oid_strat) sqlite3_finalize(mgr->stmt_insert_oid_strat);
    if (mgr->stmt_find_oid_strat)   sqlite3_finalize(mgr->stmt_find_oid_strat);
    if (mgr->stmt_cleanup_oid_strat) sqlite3_finalize(mgr->stmt_cleanup_oid_strat);
    pthread_rwlock_destroy(&mgr->orders_lock);
    free(mgr);
    tb_log_info("order manager destroyed");
}

int tb_order_mgr_load_meta(tb_order_mgr_t *mgr) {
    if (!mgr->rest) return -1;
    int count = 0;
    int rc = hl_rest_get_meta(mgr->rest, mgr->assets, &count);
    if (rc != 0) {
        tb_log_error("failed to fetch asset metadata from exchange");
        return -1;
    }
    mgr->n_assets = count;
    tb_log_info("loaded %d assets from exchange metadata", count);

    /* Log a few key assets for debugging */
    const char *important[] = {"BTC", "ETH", "SOL", "DOGE", "HYPE", NULL};
    for (int i = 0; important[i]; i++) {
        const tb_asset_meta_t *a = find_asset(mgr, important[i]);
        if (a) {
            tb_log_info("  asset: %s → id=%u, sz_decimals=%d",
                        a->name, a->asset_id, a->sz_decimals);
        }
    }
    return 0;
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

    /* Fetch asset metadata (asset IDs + size decimals) */
    if (mgr->n_assets == 0) {
        tb_order_mgr_load_meta(mgr);
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

    /* Resolve asset ID and normalize price/size precision for exchange */
    tb_order_request_t resolved = submit->order;
    if (!mgr->paper) {
        const tb_asset_meta_t *meta = find_asset(mgr, coin);
        if (!meta) {
            tb_log_error("unknown asset '%s' — not in exchange metadata", coin);
            return -1;
        }
        resolved.asset = meta->asset_id;

        /* Round price to 5 significant figures (Hyperliquid requirement) */
        double raw_price = tb_decimal_to_double(resolved.price);
        resolved.price = round_price_sigfigs(raw_price, 5);

        /* Round size to sz_decimals from metadata */
        double raw_size = tb_decimal_to_double(resolved.size);
        resolved.size = round_size(raw_size, meta->sz_decimals);

        /* Also round trigger_px if this is a trigger order */
        if (resolved.type == TB_ORDER_TRIGGER) {
            double raw_tpx = tb_decimal_to_double(resolved.trigger_px);
            resolved.trigger_px = round_price_sigfigs(raw_tpx, 5);
        }
    }

    /* Reject zero-size orders (can happen when account too small + rounding) */
    if (tb_decimal_is_zero(resolved.size)) {
        tb_log_warn("order rejected: size rounds to 0 (account too small?) strategy=%s coin=%s",
                    submit->strategy_name, coin);
        return -1;
    }

    /* Place order via paper exchange or REST */
    uint64_t oid = 0;
    int rc;
    hl_rest_fill_info_t fill_info;
    memset(&fill_info, 0, sizeof(fill_info));
    int n_filled = 0;

    if (mgr->paper) {
        rc = tb_paper_place_order(mgr->paper, &submit->order, &oid);
    } else {
        rc = hl_rest_place_orders_ex(mgr->rest, &resolved, 1,
                                      resolved.grouping,
                                      &fill_info, &n_filled);
        oid = fill_info.oid;
    }
    if (rc != 0) return -1;

    /* Cache OID → strategy (survives reconciliation + restart) */
    if (oid == 0) {
        tb_log_warn("oid=0 for %s %s (type=%d) — will use coin fallback",
                    coin, submit->strategy_name, submit->order.type);
    }
    cache_oid_strategy(mgr, oid, submit->strategy_name, coin);

    /* For trigger orders (SL/TP): also cache coin → strategy.
     * When a trigger fires, Hyperliquid creates a child order with a new OID.
     * The fill carries the child OID, not the trigger OID, so the OID→strategy
     * lookup fails. The coin-based cache is the fallback. */
    if (submit->order.type == TB_ORDER_TRIGGER) {
        cache_trigger_coin(coin, submit->strategy_name);
    }

    /* Track the order locally */
    pthread_rwlock_wrlock(&mgr->orders_lock);
    if (mgr->n_orders < MAX_OPEN_ORDERS) {
        tracked_order_t *t = &mgr->orders[mgr->n_orders];
        memset(t, 0, sizeof(*t));
        t->order.oid = oid;
        t->order.asset = resolved.asset;
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

    /* IOC fill confirmed by REST — real fill arrives via WS userFills */
    if (!mgr->paper && fill_info.filled && fill_info.avg_px > 0) {
        tb_log_debug("REST confirms fill: %s %.6f @ %.2f (oid=%llu, awaiting WS)",
                     coin, fill_info.total_sz, fill_info.avg_px,
                     (unsigned long long)oid);
    }

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

int tb_order_mgr_cancel_by_coin(tb_order_mgr_t *mgr, const char *coin,
                                 uint64_t oid) {
    const tb_asset_meta_t *meta = find_asset(mgr, coin);
    uint32_t asset = meta ? meta->asset_id : 0;
    if (!meta) {
        tb_log_warn("cancel: unknown coin '%s', using asset=0", coin);
    }
    return tb_order_mgr_cancel(mgr, asset, oid);
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

int tb_order_mgr_get_open_orders(tb_order_mgr_t *mgr, const char *coin,
                                  tb_order_t *out, int *count) {
    int max_out = *count;
    pthread_rwlock_rdlock(&mgr->orders_lock);
    int n = 0;
    for (int i = 0; i < mgr->n_orders; i++) {
        bool match = (coin == NULL || coin[0] == '\0' ||
                      strcmp(mgr->orders[i].order.coin, coin) == 0);
        if (match) {
            if (out && n < max_out) {
                out[n] = mgr->orders[i].order;
            }
            n++;
        }
    }
    *count = (n < max_out) ? n : max_out;
    pthread_rwlock_unlock(&mgr->orders_lock);
    return 0;
}

int tb_order_mgr_open_order_count(tb_order_mgr_t *mgr) {
    pthread_rwlock_rdlock(&mgr->orders_lock);
    int n = mgr->n_orders;
    pthread_rwlock_unlock(&mgr->orders_lock);
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

    /* Cleanup old OID → strategy entries (> 24h) */
    if (mgr->stmt_cleanup_oid_strat) {
        sqlite3_reset(mgr->stmt_cleanup_oid_strat);
        int64_t cutoff = ((int64_t)time(NULL) - 86400) * 1000;
        sqlite3_bind_int64(mgr->stmt_cleanup_oid_strat, 1, cutoff);
        sqlite3_step(mgr->stmt_cleanup_oid_strat);
    }

    /* Check emergency close */
    if (tb_risk_should_emergency_close(mgr->risk)) {
        tb_log_warn("EMERGENCY: daily loss limit hit, closing all positions");
        tb_order_mgr_cancel_all_coin(mgr, "");

        /* Fetch current mid prices for accurate emergency close pricing */
        tb_mid_t mids[TB_MAX_ASSETS];
        int n_mids = 0;
        if (mgr->rest) {
            hl_rest_get_all_mids(mgr->rest, mids, &n_mids);
        }

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
            /* Use current mid price ± 2% slippage for IOC.
             * Mid price tracks market reality; entry price may be far off
             * after a large move, causing IOC orders to never fill. */
            double ref_price = 0;
            for (int m = 0; m < n_mids; m++) {
                if (strcmp(mids[m].coin, positions[p].coin) == 0) {
                    ref_price = tb_decimal_to_double(mids[m].mid);
                    break;
                }
            }
            if (ref_price < 1e-9) {
                /* Fallback to entry price if mid unavailable */
                ref_price = tb_decimal_to_double(positions[p].entry_px);
            }
            if (ref_price < 1e-9) ref_price = 1.0;
            close_order.order.price = (sz > 0)
                ? tb_decimal_from_double(ref_price * 0.98, 6)
                : tb_decimal_from_double(ref_price * 1.02, 6);
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
            tb_log_warn("EMERGENCY: closing %s position sz=%.4f @ ref=%.2f",
                        positions[p].coin, sz, ref_price);
        }
    }

    free(exchange_orders);
    return 0;
}
