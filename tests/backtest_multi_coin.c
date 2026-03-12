/*
 * Multi-Coin Backtest — BB Scalping on DOGE, SOL, HYPE, BTC
 *
 * Tests the same BB Scalping strategy on multiple coins to find
 * which ones respond best. Creates temporary strategy files with
 * the coin name replaced, fetches real candles, runs walk-forward.
 *
 * Usage: ./backtest_multi_coin [end_days_ago] [n_days]
 *
 * This file is standalone and can be deleted without affecting the bot.
 */

#include "core/types.h"
#include "core/logging.h"
#include "backtest/backtest_engine.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>

/* ── ANSI ───────────────────────────────────────────────────────────────── */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_CYAN    "\033[36m"

/* ── Configuration ──────────────────────────────────────────────────────── */
#define BASE_STRATEGY   "strategies/bb_scalp_15m.lua"
#define N_DAYS_DEFAULT  30
#define INITIAL_BALANCE 100.0
#define MAX_LEVERAGE    5
#define MAKER_FEE       0.0002
#define TAKER_FEE       0.0005
#define SLIPPAGE_BPS    1.0
#define IS_SPLIT        0.60

/* Candle interval: "15m" or "1h" */
static const char *g_interval = "15m";
static int64_t     g_interval_ms = 900000LL;  /* 15min = 900s */
static int         g_candles_per_day = 96;     /* 24*4 */

/* ── Create temporary strategy file with coin replaced ─────────────────── */
static int create_temp_strategy(const char *coin, char *out_path, size_t path_len) {
    /* Read base strategy */
    FILE *f = fopen(BASE_STRATEGY, "r");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open %s\n", BASE_STRATEGY);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    /* Replace coin = "ETH" with coin = "COIN" */
    snprintf(out_path, path_len, "/tmp/scalp_%s.lua", coin);
    FILE *out = fopen(out_path, "w");
    if (!out) { free(buf); return -1; }

    char *pos = buf;
    char *found = strstr(pos, "coin          = \"ETH\"");
    if (found) {
        fwrite(pos, 1, (size_t)(found - pos), out);
        fprintf(out, "coin          = \"%s\"", coin);
        pos = found + strlen("coin          = \"ETH\"");
    }
    fwrite(pos, 1, strlen(pos), out);

    fclose(out);
    free(buf);
    return 0;
}

/* ── Load candles from SQLite cache (no network) ───────────────────────── */
#define CACHE_DB_PATH "./data/candle_cache.db"

static int fetch_candles(const char *coin, int n_days, int end_days_ago,
                          tb_candle_t *out, int max) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(CACHE_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "ERROR: cannot open cache %s\n", CACHE_DB_PATH);
        return -1;
    }

    int64_t now_s = (int64_t)time(NULL);
    int64_t end_ms = (now_s - (int64_t)end_days_ago * 86400) * 1000;
    int64_t start_ms = (end_ms / 1000 - (int64_t)n_days * 86400) * 1000;

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
    sqlite3_bind_text(stmt, 2, g_interval, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, start_ms);
    sqlite3_bind_int64(stmt, 4, end_ms);

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < max) {
        out[n].time_open = sqlite3_column_int64(stmt, 0);
        out[n].time_close = out[n].time_open + g_interval_ms;
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

/* ── Run backtest on slice ──────────────────────────────────────────────── */
static int run_slice(const char *coin, const char *strat_path,
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
    if (rc != 0) { tb_backtest_destroy(bt); return -1; }

    rc = tb_backtest_run(bt, result);
    tb_backtest_destroy(bt);
    return rc;
}

/* ── Fork-isolated runner ──────────────────────────────────────────────── */
typedef struct {
    int rc;
    tb_backtest_result_t result;
} bt_shared_t;

static int run_isolated(const char *coin, const char *strat_path,
                          const tb_candle_t *candles, int n,
                          tb_backtest_result_t *result) {
    bt_shared_t *shared = mmap(NULL, sizeof(bt_shared_t),
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED)
        return run_slice(coin, strat_path, candles, n, result);

    shared->rc = -1;
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        munmap(shared, sizeof(bt_shared_t));
        return run_slice(coin, strat_path, candles, n, result);
    }

    if (pid == 0) {
        shared->rc = run_slice(coin, strat_path, candles, n, &shared->result);
        _exit(shared->rc == 0 ? 0 : 1);
    }

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && shared->rc == 0) {
        *result = shared->result;
        munmap(shared, sizeof(bt_shared_t));
        return 0;
    }

    munmap(shared, sizeof(bt_shared_t));
    return -1;
}

/* ── Buy & Hold ─────────────────────────────────────────────────────────── */
static double buy_and_hold_return(const tb_candle_t *candles, int n) {
    if (n < 2) return 0;
    double entry = tb_decimal_to_double(candles[0].open);
    double exit_p = tb_decimal_to_double(candles[n - 1].close);
    return ((exit_p - entry) / entry) * 100.0 * MAX_LEVERAGE;
}

/* ── Verdict ────────────────────────────────────────────────────────────── */
static const char *verdict(const tb_backtest_result_t *oos, double bnh_ret) {
    if (oos->total_trades < 30)
        return C_RED "INSUFFISANT" C_RESET;
    double alpha = oos->return_pct - bnh_ret;
    if (oos->max_drawdown_pct > 50.0)
        return C_RED "ABANDON" C_RESET;
    if (oos->sharpe_ratio >= 1.5 && alpha > 0 && oos->profit_factor >= 1.5
        && oos->max_drawdown_pct < 20.0 && oos->total_trades >= 50)
        return C_GREEN "DEPLOYABLE" C_RESET;
    if (oos->sharpe_ratio >= 0.8 && oos->profit_factor >= 1.2)
        return C_YELLOW "A OPTIMISER" C_RESET;
    if (oos->net_pnl > 0)
        return C_YELLOW "MARGINAL" C_RESET;
    return C_RED "ABANDON" C_RESET;
}

/* ── Main ──────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    tb_log_init(NULL, TB_LOG_LVL_WARN);

    int end_days_ago = 0;
    int n_days = N_DAYS_DEFAULT;
    if (argc >= 2) end_days_ago = atoi(argv[1]);
    if (argc >= 3) n_days = atoi(argv[2]);
    int max_candles = n_days * g_candles_per_day + 100;

    printf("\n%s", C_BOLD);
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║           MULTI-COIN BACKTEST — BB Scalping Strategy               ║\n");
    printf("║  Periode: %d jours | Candles: %s | Balance: $%.0f | Levier: %dx     ║\n",
           n_days, g_interval, INITIAL_BALANCE, MAX_LEVERAGE);
    printf("║  Coins: BTC, SOL, DOGE, HYPE  (+ETH reference)                    ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");
    printf("%s\n", C_RESET);

    /* ── Results storage ───────────────────────────────────────────────── */
    typedef struct {
        const char *coin;
        double first_price, last_price, market_pct;
        int n_candles;
        tb_backtest_result_t full, is, oos;
        double bnh_full, bnh_is, bnh_oos;
        bool ok;
    } coin_result_t;

    /* ETH + 4 coins */
    const char *all_coins[] = { "ETH", "BTC", "SOL", "DOGE", "HYPE", "PUMP", NULL };
    coin_result_t results[6];
    memset(results, 0, sizeof(results));

    /* ── Run for each coin ─────────────────────────────────────────────── */
    for (int c = 0; all_coins[c]; c++) {
        const char *coin = all_coins[c];
        results[c].coin = coin;

        printf("%s%s── %s ──%s ", C_BOLD, C_CYAN, coin, C_RESET);
        fflush(stdout);

        /* Create temp strategy */
        char strat_path[256];
        if (strcmp(coin, "ETH") == 0) {
            strncpy(strat_path, BASE_STRATEGY, sizeof(strat_path) - 1);
        } else {
            if (create_temp_strategy(coin, strat_path, sizeof(strat_path)) != 0) {
                printf("%sSKIP (cannot create strategy)%s\n", C_RED, C_RESET);
                continue;
            }
        }

        /* Fetch candles */
        tb_candle_t *candles = calloc((size_t)max_candles, sizeof(tb_candle_t));
        if (!candles) { printf("%sOOM%s\n", C_RED, C_RESET); continue; }

        int n = fetch_candles(coin, n_days, end_days_ago, candles, max_candles);
        if (n < 50) {
            printf("%sSKIP (%d candles, need 50+)%s\n", C_RED, n, C_RESET);
            free(candles);
            if (strcmp(coin, "ETH") != 0) unlink(strat_path);
            continue;
        }

        results[c].n_candles = n;
        results[c].first_price = tb_decimal_to_double(candles[0].open);
        results[c].last_price = tb_decimal_to_double(candles[n - 1].close);
        results[c].market_pct = (results[c].last_price - results[c].first_price)
                                 / results[c].first_price * 100.0;

        printf("$%.2f → $%.2f (%+.1f%%) %d candles\n",
               results[c].first_price, results[c].last_price,
               results[c].market_pct, n);

        /* Split IS/OOS */
        int is_n = (int)(n * IS_SPLIT);
        int oos_n = n - is_n;

        /* Buy & Hold */
        results[c].bnh_full = buy_and_hold_return(candles, n);
        results[c].bnh_is = buy_and_hold_return(candles, is_n);
        results[c].bnh_oos = buy_and_hold_return(candles + is_n, oos_n);

        /* Run backtests */
        int rc_is  = run_isolated(coin, strat_path, candles, is_n, &results[c].is);
        int rc_oos = run_isolated(coin, strat_path, candles + is_n, oos_n, &results[c].oos);
        int rc_full = run_isolated(coin, strat_path, candles, n, &results[c].full);

        if (rc_full != 0) {
            /* Estimate from IS+OOS */
            results[c].full.net_pnl = results[c].is.net_pnl + results[c].oos.net_pnl;
            results[c].full.return_pct = results[c].full.net_pnl / INITIAL_BALANCE * 100.0;
            results[c].full.total_trades = results[c].is.total_trades + results[c].oos.total_trades;
            results[c].full.sharpe_ratio = (results[c].is.sharpe_ratio + results[c].oos.sharpe_ratio) / 2.0;
            results[c].full.max_drawdown_pct = fmax(results[c].is.max_drawdown_pct,
                                                     results[c].oos.max_drawdown_pct);
            results[c].full.profit_factor = (results[c].is.profit_factor + results[c].oos.profit_factor) / 2.0;
            results[c].full.win_rate = (results[c].is.win_rate + results[c].oos.win_rate) / 2.0;
        }

        results[c].ok = (rc_is == 0 && rc_oos == 0);

        if (!results[c].ok) {
            printf("  %sERROR (IS=%d OOS=%d)%s\n", C_RED, rc_is, rc_oos, C_RESET);
        } else {
            printf("  IS: %+.1f%% (%d trades)  OOS: %+.1f%% (%d trades)\n",
                   results[c].is.return_pct, results[c].is.total_trades,
                   results[c].oos.return_pct, results[c].oos.total_trades);
        }

        free(candles);
        if (strcmp(coin, "ETH") != 0) unlink(strat_path);
    }

    /* ── Comparison table ──────────────────────────────────────────────── */
    printf("\n%s", C_BOLD);
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║                    COMPARATIF MULTI-COIN (OOS)                     ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");
    printf("%s\n", C_RESET);

    printf("  %-6s %8s %8s %7s %7s %6s %5s %6s %7s  %-12s\n",
           "Coin", "OOS Ret", "Mkt Ret", "Alpha", "Sharpe", "MaxDD",
           "Trd", "WR", "PF", "Verdict");
    printf("  %-6s %8s %8s %7s %7s %6s %5s %6s %7s  %-12s\n",
           "──────", "────────", "────────", "───────", "───────",
           "──────", "─────", "──────", "───────", "────────────");

    for (int c = 0; all_coins[c]; c++) {
        if (!results[c].ok) {
            printf("  %-6s %sSKIP%s\n", results[c].coin, C_RED, C_RESET);
            continue;
        }

        const tb_backtest_result_t *oos = &results[c].oos;
        double alpha = oos->return_pct - results[c].bnh_oos;
        const char *pnl_c = oos->net_pnl >= 0 ? C_GREEN : C_RED;
        const char *alpha_c = alpha >= 0 ? C_GREEN : C_RED;
        double pf = oos->profit_factor > 999 ? 999.99 : oos->profit_factor;

        printf("  %-6s %s%+7.1f%%%s %+7.1f%% %s%+6.1f%%%s %7.2f %5.1f%% %5d %5.1f%% %7.2f  %s\n",
               results[c].coin,
               pnl_c, oos->return_pct, C_RESET,
               results[c].bnh_oos,
               alpha_c, alpha, C_RESET,
               oos->sharpe_ratio,
               oos->max_drawdown_pct,
               oos->total_trades,
               oos->win_rate,
               pf,
               verdict(oos, results[c].bnh_oos));
    }

    printf("\n  %sStrategie: BB Scalping (BB20/2.0, RSI 35/65, SL 1.5%%, TP 3.0%%)%s\n",
           C_DIM, C_RESET);
    printf("  %sPeriode: %d jours, Candles %s, Balance $%.0f, Levier %dx%s\n\n",
           C_DIM, n_days, g_interval, INITIAL_BALANCE, MAX_LEVERAGE, C_RESET);

    return 0;
}
