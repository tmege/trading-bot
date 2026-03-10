/*
 * Backtest JSON — Machine-readable backtest output
 *
 * Same logic as backtest_real.c but outputs structured JSON on stdout
 * for consumption by the Electron GUI.
 *
 * Usage: ./backtest_json <strategy.lua> <coin> <end_days_ago> <n_days> [interval]
 * Output: JSON object with config, stats, trades[], equity_curve[], verdict
 */

#include "core/types.h"
#include "core/logging.h"
#include "exchange/hl_rest.h"
#include "backtest/backtest_engine.h"

#include <yyjson.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>

/* ── Defaults ──────────────────────────────────────────────────────────── */
#define INITIAL_BALANCE 100.0
#define MAX_LEVERAGE    5
#define MAKER_FEE       0.0002
#define TAKER_FEE       0.0005
#define SLIPPAGE_BPS    1.0
#define IS_SPLIT        0.60
#define CACHE_DB_PATH   "./data/candle_cache.db"

/* ── Progress output to stderr (stdout is reserved for JSON) ──────────── */
static void progress(const char *status, const char *detail) {
    fprintf(stderr, "{\"status\":\"%s\",\"detail\":\"%s\"}\n", status, detail);
    fflush(stderr);
}

/* ── Load candles from SQLite cache ───────────────────────────────────── */
static int load_from_cache(const char *coin, const char *interval,
                            int64_t start_ms, int64_t end_ms,
                            tb_candle_t *out, int max) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(CACHE_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        return -1;
    }

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT time_ms, open, high, low, close, volume FROM candles "
        "WHERE coin=? AND interval=? AND time_ms>=? AND time_ms<=? "
        "ORDER BY time_ms ASC",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, coin, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, interval, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, start_ms);
    sqlite3_bind_int64(stmt, 4, end_ms);

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < max) {
        out[n].time_open = sqlite3_column_int64(stmt, 0);
        out[n].time_close = out[n].time_open;
        out[n].open   = tb_decimal_from_double(sqlite3_column_double(stmt, 1), 8);
        out[n].high   = tb_decimal_from_double(sqlite3_column_double(stmt, 2), 8);
        out[n].low    = tb_decimal_from_double(sqlite3_column_double(stmt, 3), 8);
        out[n].close  = tb_decimal_from_double(sqlite3_column_double(stmt, 4), 8);
        out[n].volume = tb_decimal_from_double(sqlite3_column_double(stmt, 5), 8);
        n++;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return n;
}

/* ── Fetch candles from Hyperliquid API (fallback) ────────────────────── */
static int fetch_from_api(const char *coin, const char *interval,
                           int64_t start_ms, int64_t end_ms,
                           tb_candle_t *out, int max) {
    hl_rest_t *rest = hl_rest_create("https://api.hyperliquid.xyz", NULL, true);
    if (!rest) return -1;

    int64_t candle_ms = 3600000LL;
    if (strcmp(interval, "5m") == 0)  candle_ms = 300000LL;
    if (strcmp(interval, "15m") == 0) candle_ms = 900000LL;
    if (strcmp(interval, "4h") == 0)  candle_ms = 14400000LL;
    if (strcmp(interval, "1d") == 0)  candle_ms = 86400000LL;

    int total = 0;
    int64_t cursor = start_ms;

    while (cursor < end_ms && total < max) {
        int batch_count = 0;
        int rc = hl_rest_get_candles(rest, coin, interval,
                                      cursor, end_ms,
                                      &out[total], &batch_count,
                                      max - total);
        if (rc != 0 || batch_count == 0) {
            if (total > 0) break;
            hl_rest_destroy(rest);
            return -1;
        }

        total += batch_count;
        cursor = out[total - 1].time_open + candle_ms + 1;
        if (cursor < end_ms) usleep(200000);
    }

    hl_rest_destroy(rest);
    return total;
}

/* ── Fetch candles: cache first, API fallback ─────────────────────────── */
static int fetch_candles(const char *coin, int n_days, int end_days_ago,
                         const char *interval, tb_candle_t *out, int max) {
    int64_t now_s = (int64_t)time(NULL);
    int64_t end_ms = (now_s - (int64_t)end_days_ago * 86400) * 1000;
    int64_t start_ms = (end_ms / 1000 - (int64_t)n_days * 86400) * 1000;

    /* Try cache first */
    int n = load_from_cache(coin, interval, start_ms, end_ms, out, max);
    if (n >= 50) {
        fprintf(stderr, "{\"status\":\"cache\",\"detail\":\"%s %d candles from cache\"}\n",
                coin, n);
        return n;
    }

    /* Fallback to API */
    fprintf(stderr, "{\"status\":\"fetching\",\"detail\":\"%s from API\"}\n", coin);
    return fetch_from_api(coin, interval, start_ms, end_ms, out, max);
}

/* ── Buy & Hold benchmark ─────────────────────────────────────────────── */
typedef struct {
    double return_pct;
    double sharpe;
    double sortino;
    double max_dd_pct;
} bnh_result_t;

static bnh_result_t compute_buy_and_hold(const tb_candle_t *candles, int n,
                                          double initial_balance, int leverage) {
    bnh_result_t r = {0};
    if (n < 2) return r;

    double entry = tb_decimal_to_double(candles[0].open);
    double fee_entry = entry * TAKER_FEE;
    double size = (initial_balance * leverage) / (entry + fee_entry);

    double *period_ret = calloc((size_t)n, sizeof(double));
    double peak = initial_balance;
    double max_dd = 0;
    double prev_equity = initial_balance;

    for (int i = 1; i < n; i++) {
        double price = tb_decimal_to_double(candles[i].close);
        double pnl = (price - entry) * size;
        double equity = initial_balance + pnl;

        if (equity > peak) peak = equity;
        double dd = (peak - equity) / peak * 100.0;
        if (dd > max_dd) max_dd = dd;

        period_ret[i] = (equity - prev_equity) / prev_equity;
        prev_equity = equity;
    }

    double exit_price = tb_decimal_to_double(candles[n - 1].close);
    double fee_exit = exit_price * TAKER_FEE;
    double total_pnl = (exit_price - entry) * size - (fee_entry + fee_exit) * size;
    r.return_pct = total_pnl / initial_balance * 100.0;
    r.max_dd_pct = max_dd;

    double sum = 0, sum2 = 0, sum_neg2 = 0;
    int count = 0;
    for (int i = 1; i < n; i++) {
        sum += period_ret[i];
        sum2 += period_ret[i] * period_ret[i];
        if (period_ret[i] < 0) sum_neg2 += period_ret[i] * period_ret[i];
        count++;
    }

    if (count > 1) {
        double mean = sum / count;
        double var = sum2 / count - mean * mean;
        double std = sqrt(var > 0 ? var : 0);
        double downside_std = sqrt(sum_neg2 / count);

        /* Annualize based on candle frequency */
        double ann = sqrt(8760.0); /* hourly default */
        r.sharpe = std > 0 ? (mean / std) * ann : 0;
        r.sortino = downside_std > 0 ? (mean / downside_std) * ann : 0;
    }

    free(period_ret);
    return r;
}

/* ── Run backtest on a candle slice ────────────────────────────────────── */
static int run_backtest_on_slice(const char *strat_path, const char *coin,
                                  const tb_candle_t *candles, int n,
                                  tb_backtest_result_t *result) {
    tb_backtest_config_t cfg = {
        .coin            = coin,
        .strategy_path   = strat_path,
        .initial_balance = INITIAL_BALANCE,
        .max_leverage    = MAX_LEVERAGE,
        .maker_fee_rate  = MAKER_FEE,
        .taker_fee_rate  = TAKER_FEE,
        .slippage_bps    = SLIPPAGE_BPS,
    };

    tb_backtest_engine_t *bt = tb_backtest_create(&cfg);
    if (!bt) return -1;

    int rc = tb_backtest_load_candles(bt, candles, n);
    if (rc != 0) {
        tb_backtest_destroy(bt);
        return -1;
    }

    rc = tb_backtest_run(bt, result);
    tb_backtest_destroy(bt);
    return rc;
}

/* ── Fork-isolated backtest ───────────────────────────────────────────── */
typedef struct {
    int rc;
    tb_backtest_result_t result;
} bt_shared_t;

static int run_backtest_isolated(const char *strat_path, const char *coin,
                                  const tb_candle_t *candles, int n,
                                  tb_backtest_result_t *result) {
    bt_shared_t *shared = mmap(NULL, sizeof(bt_shared_t),
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        return run_backtest_on_slice(strat_path, coin, candles, n, result);
    }
    shared->rc = -1;

    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        munmap(shared, sizeof(bt_shared_t));
        return run_backtest_on_slice(strat_path, coin, candles, n, result);
    }

    if (pid == 0) {
        shared->rc = run_backtest_on_slice(strat_path, coin, candles, n,
                                            &shared->result);
        if (shared->rc != 0) {
            fprintf(stderr, "{\"status\":\"debug\",\"detail\":\"child slice failed rc=%d n=%d\"}\n",
                    shared->rc, n);
        }
        _exit(shared->rc == 0 ? 0 : 1);
    }

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && shared->rc == 0) {
        *result = shared->result;
        munmap(shared, sizeof(bt_shared_t));
        return 0;
    }

    if (WIFSIGNALED(status)) {
        fprintf(stderr, "{\"status\":\"debug\",\"detail\":\"child killed by signal %d, n=%d\"}\n",
                WTERMSIG(status), n);
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "{\"status\":\"debug\",\"detail\":\"child exited code=%d, n=%d\"}\n",
                WEXITSTATUS(status), n);
    }

    munmap(shared, sizeof(bt_shared_t));
    return -1;
}

/* ── Verdict logic (returns plain text, no ANSI) ──────────────────────── */
static const char *verdict_str(const tb_backtest_result_t *oos,
                                const bnh_result_t *bnh_oos) {
    if (oos->total_trades < 30) return "INSUFFISANT";

    double alpha = oos->return_pct - bnh_oos->return_pct;

    if (oos->max_drawdown_pct > 50.0)
        return "ABANDON";

    if (oos->sharpe_ratio < 0.5 && alpha < 0)
        return "ABANDON";

    if (oos->sharpe_ratio >= 1.5 && alpha > 0 && oos->profit_factor >= 1.5
        && oos->max_drawdown_pct < 20.0 && oos->total_trades >= 50)
        return "DEPLOYABLE";

    if (oos->sharpe_ratio >= 0.8 && oos->profit_factor >= 1.2 && alpha > -5.0)
        return "A_OPTIMISER";

    if (oos->net_pnl > 0 && oos->profit_factor >= 1.0)
        return "MARGINAL";

    return "ABANDON";
}

/* ── Add backtest result stats to JSON object ─────────────────────────── */
static void add_stats_to_json(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                               const tb_backtest_result_t *r) {
    yyjson_mut_obj_add_real(doc, obj, "net_pnl", r->net_pnl);
    yyjson_mut_obj_add_real(doc, obj, "return_pct", r->return_pct);
    yyjson_mut_obj_add_real(doc, obj, "total_pnl", r->total_pnl);
    yyjson_mut_obj_add_real(doc, obj, "total_fees", r->total_fees);
    yyjson_mut_obj_add_int(doc, obj, "total_trades", r->total_trades);
    yyjson_mut_obj_add_int(doc, obj, "winning_trades", r->winning_trades);
    yyjson_mut_obj_add_int(doc, obj, "losing_trades", r->losing_trades);
    yyjson_mut_obj_add_real(doc, obj, "win_rate", r->win_rate);
    yyjson_mut_obj_add_real(doc, obj, "profit_factor",
                             r->profit_factor > 9999 ? 9999.0 : r->profit_factor);
    yyjson_mut_obj_add_real(doc, obj, "avg_win", r->avg_win);
    yyjson_mut_obj_add_real(doc, obj, "avg_loss", r->avg_loss);
    yyjson_mut_obj_add_real(doc, obj, "max_win", r->max_win);
    yyjson_mut_obj_add_real(doc, obj, "max_loss", r->max_loss);
    yyjson_mut_obj_add_real(doc, obj, "sharpe_ratio", r->sharpe_ratio);
    yyjson_mut_obj_add_real(doc, obj, "sortino_ratio", r->sortino_ratio);
    yyjson_mut_obj_add_real(doc, obj, "max_drawdown_pct", r->max_drawdown_pct);
    yyjson_mut_obj_add_real(doc, obj, "start_balance", r->start_balance);
    yyjson_mut_obj_add_real(doc, obj, "end_balance", r->end_balance);
    yyjson_mut_obj_add_int(doc, obj, "n_candles", r->n_candles);
    yyjson_mut_obj_add_int(doc, obj, "n_days", r->n_days);
}

/* ── Add B&H stats to JSON ────────────────────────────────────────────── */
static void add_bnh_to_json(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                             const bnh_result_t *b) {
    yyjson_mut_obj_add_real(doc, obj, "return_pct", b->return_pct);
    yyjson_mut_obj_add_real(doc, obj, "sharpe", b->sharpe);
    yyjson_mut_obj_add_real(doc, obj, "sortino", b->sortino);
    yyjson_mut_obj_add_real(doc, obj, "max_dd_pct", b->max_dd_pct);
}

/* ── Main ──────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    tb_log_init("./logs", TB_LOG_LVL_WARN);

    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s <strategy.lua> <coin> <end_days_ago> <n_days> [interval]\n",
            argv[0]);
        return 1;
    }

    const char *strategy_path = argv[1];
    const char *coin          = argv[2];
    int end_days_ago          = atoi(argv[3]);
    int n_days                = atoi(argv[4]);
    const char *interval      = argc >= 6 ? argv[5] : "1h";

    /* Compute max candles based on interval */
    int candles_per_day = 24; /* 1h */
    if (strcmp(interval, "5m") == 0)  candles_per_day = 288;
    else if (strcmp(interval, "15m") == 0) candles_per_day = 96;
    else if (strcmp(interval, "4h") == 0) candles_per_day = 6;
    else if (strcmp(interval, "1d") == 0) candles_per_day = 1;
    int max_candles = n_days * candles_per_day + 200;

    /* ── Fetch candles ─────────────────────────────────────────────────── */
    progress("fetching", coin);

    tb_candle_t *candles = calloc((size_t)max_candles, sizeof(tb_candle_t));
    if (!candles) {
        fprintf(stderr, "{\"status\":\"error\",\"detail\":\"malloc failed\"}\n");
        return 1;
    }

    int n_candles = fetch_candles(coin, n_days, end_days_ago, interval,
                                   candles, max_candles);
    if (n_candles < 50) {
        fprintf(stderr, "{\"status\":\"error\",\"detail\":\"only got %d candles\"}\n",
                n_candles);
        free(candles);
        return 1;
    }

    /* ── Split IS/OOS ──────────────────────────────────────────────────── */
    progress("running", "walk-forward split");

    int is_count = (int)(n_candles * IS_SPLIT);
    int oos_count = n_candles - is_count;
    const tb_candle_t *is_candles = candles;
    const tb_candle_t *oos_candles = candles + is_count;

    /* ── Run backtests ─────────────────────────────────────────────────── */
    progress("running", "in-sample backtest");
    tb_backtest_result_t r_full, r_is, r_oos;
    memset(&r_full, 0, sizeof(r_full));
    memset(&r_is, 0, sizeof(r_is));
    memset(&r_oos, 0, sizeof(r_oos));

    int rc_is = run_backtest_isolated(strategy_path, coin,
                                       is_candles, is_count, &r_is);

    progress("running", "out-of-sample backtest");
    int rc_oos = run_backtest_isolated(strategy_path, coin,
                                        oos_candles, oos_count, &r_oos);

    if (rc_is != 0 || rc_oos != 0) {
        fprintf(stderr, "{\"status\":\"error\",\"detail\":\"backtest failed IS=%d OOS=%d\"}\n",
                rc_is, rc_oos);
        free(candles);
        return 1;
    }

    progress("running", "full period backtest");
    int rc_full = run_backtest_isolated(strategy_path, coin,
                                         candles, n_candles, &r_full);
    if (rc_full != 0) {
        /* Estimate from IS + OOS */
        r_full.net_pnl = r_is.net_pnl + r_oos.net_pnl;
        r_full.return_pct = r_full.net_pnl / INITIAL_BALANCE * 100.0;
        r_full.total_trades = r_is.total_trades + r_oos.total_trades;
        r_full.sharpe_ratio = (r_is.sharpe_ratio + r_oos.sharpe_ratio) / 2.0;
        r_full.sortino_ratio = (r_is.sortino_ratio + r_oos.sortino_ratio) / 2.0;
        r_full.max_drawdown_pct = r_is.max_drawdown_pct > r_oos.max_drawdown_pct ?
                                  r_is.max_drawdown_pct : r_oos.max_drawdown_pct;
        r_full.win_rate = (r_is.win_rate + r_oos.win_rate) / 2.0;
        r_full.profit_factor = (r_is.profit_factor + r_oos.profit_factor) / 2.0;
        r_full.total_fees = r_is.total_fees + r_oos.total_fees;
    }

    /* ── Buy & Hold benchmarks ─────────────────────────────────────────── */
    bnh_result_t bnh_full = compute_buy_and_hold(candles, n_candles,
                                                  INITIAL_BALANCE, MAX_LEVERAGE);
    bnh_result_t bnh_is   = compute_buy_and_hold(is_candles, is_count,
                                                  INITIAL_BALANCE, MAX_LEVERAGE);
    bnh_result_t bnh_oos  = compute_buy_and_hold(oos_candles, oos_count,
                                                  INITIAL_BALANCE, MAX_LEVERAGE);

    /* ── Build JSON output ─────────────────────────────────────────────── */
    progress("complete", "building JSON");

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    /* Config */
    yyjson_mut_val *cfg = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, cfg, "strategy", strategy_path);
    yyjson_mut_obj_add_str(doc, cfg, "coin", coin);
    yyjson_mut_obj_add_str(doc, cfg, "interval", interval);
    yyjson_mut_obj_add_int(doc, cfg, "n_days", n_days);
    yyjson_mut_obj_add_int(doc, cfg, "end_days_ago", end_days_ago);
    yyjson_mut_obj_add_real(doc, cfg, "initial_balance", INITIAL_BALANCE);
    yyjson_mut_obj_add_int(doc, cfg, "max_leverage", MAX_LEVERAGE);
    yyjson_mut_obj_add_real(doc, cfg, "maker_fee", MAKER_FEE);
    yyjson_mut_obj_add_real(doc, cfg, "taker_fee", TAKER_FEE);
    yyjson_mut_obj_add_real(doc, cfg, "slippage_bps", SLIPPAGE_BPS);
    yyjson_mut_obj_add_int(doc, cfg, "n_candles", n_candles);
    yyjson_mut_obj_add_val(doc, root, "config", cfg);

    /* Stats — full period */
    yyjson_mut_val *stats = yyjson_mut_obj(doc);
    add_stats_to_json(doc, stats, &r_full);
    yyjson_mut_obj_add_val(doc, root, "stats", stats);

    /* Stats — in-sample */
    yyjson_mut_val *stats_is = yyjson_mut_obj(doc);
    add_stats_to_json(doc, stats_is, &r_is);
    yyjson_mut_obj_add_val(doc, root, "stats_is", stats_is);

    /* Stats — out-of-sample */
    yyjson_mut_val *stats_oos = yyjson_mut_obj(doc);
    add_stats_to_json(doc, stats_oos, &r_oos);
    yyjson_mut_obj_add_val(doc, root, "stats_oos", stats_oos);

    /* Buy & Hold */
    yyjson_mut_val *bnh = yyjson_mut_obj(doc);
    yyjson_mut_val *bnh_f = yyjson_mut_obj(doc);
    add_bnh_to_json(doc, bnh_f, &bnh_full);
    yyjson_mut_val *bnh_i = yyjson_mut_obj(doc);
    add_bnh_to_json(doc, bnh_i, &bnh_is);
    yyjson_mut_val *bnh_o = yyjson_mut_obj(doc);
    add_bnh_to_json(doc, bnh_o, &bnh_oos);
    yyjson_mut_obj_add_val(doc, bnh, "full", bnh_f);
    yyjson_mut_obj_add_val(doc, bnh, "is", bnh_i);
    yyjson_mut_obj_add_val(doc, bnh, "oos", bnh_o);
    yyjson_mut_obj_add_val(doc, root, "buy_and_hold", bnh);

    /* Walk-forward analysis */
    yyjson_mut_val *wf = yyjson_mut_obj(doc);
    double sharpe_decay = r_is.sharpe_ratio > 0 ?
        (r_oos.sharpe_ratio - r_is.sharpe_ratio) / r_is.sharpe_ratio * 100.0 : 0;
    double pf_decay = (r_is.profit_factor > 0 && r_is.profit_factor < 9000) ?
        (fmin(r_oos.profit_factor, 9999) - r_is.profit_factor) / r_is.profit_factor * 100.0 : 0;
    double wr_decay = r_oos.win_rate - r_is.win_rate;

    yyjson_mut_obj_add_real(doc, wf, "sharpe_decay_pct", sharpe_decay);
    yyjson_mut_obj_add_real(doc, wf, "pf_decay_pct", pf_decay);
    yyjson_mut_obj_add_real(doc, wf, "wr_decay_pp", wr_decay);
    yyjson_mut_obj_add_bool(doc, wf, "overfit_warning",
                             (sharpe_decay < -50) || (pf_decay < -50));
    yyjson_mut_obj_add_val(doc, root, "walk_forward", wf);

    /* Trades array (from full period result) */
    yyjson_mut_val *trades = yyjson_mut_arr(doc);
    const tb_backtest_result_t *src = (rc_full == 0) ? &r_full : &r_oos;
    for (int i = 0; i < src->n_trade_log; i++) {
        yyjson_mut_val *t = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, t, "time_ms", src->trades[i].time_ms);
        yyjson_mut_obj_add_str(doc, t, "side", src->trades[i].side);
        yyjson_mut_obj_add_real(doc, t, "price", src->trades[i].price);
        yyjson_mut_obj_add_real(doc, t, "size", src->trades[i].size);
        yyjson_mut_obj_add_real(doc, t, "pnl", src->trades[i].pnl);
        yyjson_mut_obj_add_real(doc, t, "fee", src->trades[i].fee);
        yyjson_mut_obj_add_real(doc, t, "balance_after", src->trades[i].balance_after);
        yyjson_mut_arr_append(trades, t);
    }
    yyjson_mut_obj_add_val(doc, root, "trades", trades);

    /* Equity curve */
    yyjson_mut_val *equity = yyjson_mut_arr(doc);
    for (int i = 0; i < src->n_equity_points; i++) {
        yyjson_mut_val *pt = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, pt, "time_ms", src->equity_curve[i].time_ms);
        yyjson_mut_obj_add_real(doc, pt, "equity", src->equity_curve[i].equity);
        yyjson_mut_obj_add_real(doc, pt, "drawdown", src->equity_curve[i].drawdown);
        yyjson_mut_arr_append(equity, pt);
    }
    yyjson_mut_obj_add_val(doc, root, "equity_curve", equity);

    /* Verdict */
    yyjson_mut_obj_add_str(doc, root, "verdict", verdict_str(&r_oos, &bnh_oos));

    /* Alpha */
    yyjson_mut_obj_add_real(doc, root, "alpha",
                             r_oos.return_pct - bnh_oos.return_pct);

    /* ── Write JSON to stdout ──────────────────────────────────────────── */
    char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, NULL);
    if (json) {
        fputs(json, stdout);
        fputc('\n', stdout);
        free(json);
    }

    yyjson_mut_doc_free(doc);
    free(candles);
    return 0;
}
