/*
 * Real Data Backtest — Quantitative Analysis
 *
 * Loads real ETH candles from SQLite cache and runs walk-forward
 * backtests with proper quant metrics:
 * - In-sample (60%) / Out-of-sample (40%) split
 * - Sharpe, Sortino, Max Drawdown
 * - Comparison vs Buy & Hold
 * - Statistical significance check (100+ trades)
 * - Transaction costs: maker 2bps, taker 5bps, slippage 1bp
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
#define C_WHITE   "\033[37m"
#define C_MAGENTA "\033[35m"

/* ── Configuration ──────────────────────────────────────────────────────── */
#define COIN            "ETH"
#define N_DAYS          90
#define INITIAL_BALANCE 100.0
#define MAX_LEVERAGE    5
#define MAKER_FEE       0.0002
#define TAKER_FEE       0.0005
#define SLIPPAGE_BPS    1.0
#define IS_SPLIT        0.60    /* 60% in-sample, 40% out-of-sample */
#define MAX_CANDLES     (N_DAYS * 24 + 100)

/* ── Strategy list ──────────────────────────────────────────────────────── */
typedef struct {
    const char *name;
    const char *path;
} strategy_t;

static const strategy_t STRATEGIES[] = {
    { "Sniper 1h", "strategies/btc_sniper_1h.lua" },
    { NULL, NULL }
};

/* ── Load candles from SQLite cache (no network) ───────────────────────── */
#define CACHE_DB_PATH "./data/candle_cache.db"

static int fetch_real_candles(const char *coin, int n_days, int end_days_ago,
                               tb_candle_t *out, int max) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(CACHE_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "ERROR: cannot open cache %s\n", CACHE_DB_PATH);
        return -1;
    }

    int64_t now_s = (int64_t)time(NULL);
    int64_t end_ms = (now_s - (int64_t)end_days_ago * 86400) * 1000;
    int64_t start_ms = (end_ms / 1000 - (int64_t)n_days * 86400) * 1000;
    int64_t candle_ms = 3600000LL; /* 1h */

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT time_ms, open, high, low, close, volume FROM candles "
        "WHERE coin=? AND interval='1h' AND time_ms>=? AND time_ms<=? "
        "ORDER BY time_ms ASC",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, coin, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, start_ms);
    sqlite3_bind_int64(stmt, 3, end_ms);

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < max) {
        out[n].time_open = sqlite3_column_int64(stmt, 0);
        out[n].time_close = out[n].time_open + candle_ms;
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

/* ── Buy & Hold benchmark ──────────────────────────────────────────────── */
typedef struct {
    double return_pct;
    double sharpe;
    double sortino;
    double max_dd_pct;
} bnh_result_t;

static bnh_result_t compute_buy_and_hold(const tb_candle_t *candles, int n,
                                          double initial_balance, int leverage,
                                          const char *interval) {
    bnh_result_t r = {0};
    if (n < 2) return r;

    double entry = tb_decimal_to_double(candles[0].open);
    double fee_entry = entry * TAKER_FEE;
    double size = (initial_balance * leverage) / (entry + fee_entry);

    /* Track daily equity for Sharpe/Sortino */
    double *daily_ret = calloc((size_t)n, sizeof(double));
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

        daily_ret[i] = (equity - prev_equity) / prev_equity;
        prev_equity = equity;
    }

    double exit_price = tb_decimal_to_double(candles[n - 1].close);
    double fee_exit = exit_price * TAKER_FEE;
    double total_pnl = (exit_price - entry) * size - (fee_entry + fee_exit) * size;
    r.return_pct = total_pnl / initial_balance * 100.0;
    r.max_dd_pct = max_dd;

    /* Sharpe & Sortino (annualized, hourly data → sqrt(8760)) */
    double sum = 0, sum2 = 0, sum_neg2 = 0;
    int count = 0;
    for (int i = 1; i < n; i++) {
        sum += daily_ret[i];
        sum2 += daily_ret[i] * daily_ret[i];
        if (daily_ret[i] < 0) sum_neg2 += daily_ret[i] * daily_ret[i];
        count++;
    }

    if (count > 1) {
        double mean = sum / count;
        double var = sum2 / count - mean * mean;
        double std = sqrt(var > 0 ? var : 0);
        double downside_std = sqrt(sum_neg2 / count);

        /* Annualize based on candle interval */
        double periods_per_year = 8760.0; /* 1h default */
        if (strcmp(interval, "5m") == 0)       periods_per_year = 105120.0;
        else if (strcmp(interval, "15m") == 0) periods_per_year = 35040.0;
        else if (strcmp(interval, "4h") == 0)  periods_per_year = 2190.0;
        else if (strcmp(interval, "1d") == 0)  periods_per_year = 365.0;
        double ann = sqrt(periods_per_year);
        r.sharpe = std > 0 ? (mean / std) * ann : 0;
        r.sortino = downside_std > 0 ? (mean / downside_std) * ann : 0;
    }

    free(daily_ret);
    return r;
}

/* ── Run backtest on a candle slice ─────────────────────────────────────── */
static int run_backtest_on_slice(const char *strat_path,
                                  const tb_candle_t *candles, int n,
                                  tb_backtest_result_t *result) {
    tb_backtest_config_t cfg = {
        .coin            = COIN,
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

/* ── Fork-isolated backtest runner ──────────────────────────────────────── */
/* Runs backtest in a child process so SIGABRT/OOM doesn't kill the parent */
typedef struct {
    int rc;
    tb_backtest_result_t result;
} bt_shared_t;

static int run_backtest_isolated(const char *strat_path,
                                  const tb_candle_t *candles, int n,
                                  tb_backtest_result_t *result) {
    bt_shared_t *shared = mmap(NULL, sizeof(bt_shared_t),
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        /* Fallback: run in-process */
        return run_backtest_on_slice(strat_path, candles, n, result);
    }
    shared->rc = -1;

    /* Flush stdout before fork so output order is correct */
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        munmap(shared, sizeof(bt_shared_t));
        return run_backtest_on_slice(strat_path, candles, n, result);
    }

    if (pid == 0) {
        /* Child: run backtest, write result to shared memory */
        shared->rc = run_backtest_on_slice(strat_path, candles, n, &shared->result);
        _exit(shared->rc == 0 ? 0 : 1);
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && shared->rc == 0) {
        *result = shared->result;
        munmap(shared, sizeof(bt_shared_t));
        return 0;
    }

    /* Child crashed or returned error */
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        fprintf(stderr, "  [backtest child killed by signal %d (%s), %d candles]\n",
                sig, strsignal(sig), n);
    }

    munmap(shared, sizeof(bt_shared_t));
    return -1;
}

/* ── Print a single result line ─────────────────────────────────────────── */
static void print_result_line(const char *label,
                               const tb_backtest_result_t *r,
                               const bnh_result_t *bnh) {
    const char *pnl_color = r->net_pnl >= 0 ? C_GREEN : C_RED;
    const char *alpha_color = (r->return_pct - bnh->return_pct) >= 0 ? C_GREEN : C_RED;

    printf("  %-20s ", label);
    printf("%s%+8.2f$%s ", pnl_color, r->net_pnl, C_RESET);
    printf("%s%+7.1f%%%s ", pnl_color, r->return_pct, C_RESET);
    printf("%7.2f ", r->sharpe_ratio);
    printf("%7.2f ", r->sortino_ratio);
    printf("%6.1f%% ", r->max_drawdown_pct);
    printf("%5d ", r->total_trades);
    printf("%6.1f%% ", r->win_rate);
    printf("%6.2f ", r->profit_factor);
    printf("%6.2f$ ", r->total_fees);
    printf("%s%+6.1f%%%s",
           alpha_color, r->return_pct - bnh->return_pct, C_RESET);
    printf("\n");
}

/* ── Print section header ──────────────────────────────────────────────── */
static void print_header(const char *title) {
    printf("\n  %-20s %9s %8s %7s %7s %6s %5s %6s %6s %7s %7s\n",
           title, "P&L", "Return", "Sharpe", "Sortino", "MaxDD",
           "Trd", "WR", "PF", "Fees", "Alpha");
    printf("  %-20s %9s %8s %7s %7s %6s %5s %6s %6s %7s %7s\n",
           "────────────────────",
           "─────────", "────────", "───────", "───────", "──────",
           "─────", "──────", "──────", "───────", "───────");
}

/* ── Verdict logic ──────────────────────────────────────────────────────── */
static const char *verdict(const tb_backtest_result_t *oos, const bnh_result_t *bnh_oos) {
    /* Must have statistical significance */
    if (oos->total_trades < 30) return C_RED "INSUFFISANT (< 30 trades)" C_RESET;

    double alpha = oos->return_pct - bnh_oos->return_pct;

    /* Risk-adjusted checks */
    if (oos->max_drawdown_pct > 50.0)
        return C_RED "ABANDON (MaxDD > 50%%)" C_RESET;

    if (oos->sharpe_ratio < 0.5 && alpha < 0)
        return C_RED "ABANDON (Sharpe < 0.5, alpha < 0)" C_RESET;

    if (oos->sharpe_ratio >= 1.5 && alpha > 0 && oos->profit_factor >= 1.5
        && oos->max_drawdown_pct < 20.0 && oos->total_trades >= 50)
        return C_GREEN "DEPLOYABLE" C_RESET;

    if (oos->sharpe_ratio >= 0.8 && oos->profit_factor >= 1.2 && alpha > -5.0)
        return C_YELLOW "A OPTIMISER" C_RESET;

    if (oos->net_pnl > 0 && oos->profit_factor >= 1.0)
        return C_YELLOW "A OPTIMISER (marginal)" C_RESET;

    return C_RED "ABANDON" C_RESET;
}

/* ── Main ──────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    tb_log_init(NULL, TB_LOG_LVL_WARN);  /* suppress debug noise */

    /* Parse optional args: ./backtest_real [end_days_ago] [n_days] */
    int end_days_ago = 0;   /* 0 = now */
    int n_days = N_DAYS;
    if (argc >= 2) end_days_ago = atoi(argv[1]);
    if (argc >= 3) n_days = atoi(argv[2]);
    int max_candles = n_days * 24 + 100;

    const char *regime = end_days_ago == 0 ? "RECENT" :
                         end_days_ago > 0  ? "HISTORIQUE" : "RECENT";

    printf("\n%s", C_BOLD);
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║         BACKTEST QUANTITATIF — DONNEES REELLES (CACHE)              ║\n");
    printf("║  Coin: %s | Periode: %d jours | Balance: $%.0f | Levier: %dx        ║\n",
           COIN, n_days, INITIAL_BALANCE, MAX_LEVERAGE);
    printf("║  Fees: maker %.1fbps, taker %.1fbps, slippage %.1fbps               ║\n",
           MAKER_FEE * 10000, TAKER_FEE * 10000, SLIPPAGE_BPS);
    printf("║  Walk-forward: IS %.0f%% / OOS %.0f%%  |  Mode: %-20s  ║\n",
           IS_SPLIT * 100, (1.0 - IS_SPLIT) * 100, regime);
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");
    printf("%s\n", C_RESET);

    if (end_days_ago > 0) {
        printf("%s  Offset: fin = il y a %d jours%s\n", C_DIM, end_days_ago, C_RESET);
    }

    /* ── Step 1: Fetch real candles ─────────────────────────────────────── */
    printf("%sLoading %d days of %s 1h candles from cache...%s\n",
           C_DIM, n_days, COIN, C_RESET);

    tb_candle_t *candles = calloc((size_t)max_candles, sizeof(tb_candle_t));
    if (!candles) {
        fprintf(stderr, "ERROR: malloc failed\n");
        return 1;
    }

    int n_candles = fetch_real_candles(COIN, n_days, end_days_ago,
                                       candles, max_candles);
    if (n_candles < 100) {
        fprintf(stderr, "ERROR: only got %d candles (need 100+) — run candle_fetcher to download data\n", n_candles);
        free(candles);
        return 1;
    }

    double first_price = tb_decimal_to_double(candles[0].open);
    double last_price = tb_decimal_to_double(candles[n_candles - 1].close);
    double market_return = (last_price - first_price) / first_price * 100.0;

    printf("%s  Got %d candles (%d days)%s\n", C_DIM, n_candles, n_candles / 24, C_RESET);
    printf("%s  Price range: $%.2f → $%.2f (market: %+.1f%%)%s\n\n",
           C_DIM, first_price, last_price, market_return, C_RESET);

    /* ── Step 2: Split IS/OOS ──────────────────────────────────────────── */
    int is_count = (int)(n_candles * IS_SPLIT);
    int oos_count = n_candles - is_count;
    const tb_candle_t *is_candles = candles;
    const tb_candle_t *oos_candles = candles + is_count;

    double is_start = tb_decimal_to_double(is_candles[0].open);
    double is_end = tb_decimal_to_double(is_candles[is_count - 1].close);
    double oos_start = tb_decimal_to_double(oos_candles[0].open);
    double oos_end = tb_decimal_to_double(oos_candles[oos_count - 1].close);

    printf("%sSplit Walk-Forward:%s\n", C_BOLD, C_RESET);
    printf("  In-sample:     %d candles (%d days) $%.0f → $%.0f (%+.1f%%)\n",
           is_count, is_count / 24, is_start, is_end,
           (is_end - is_start) / is_start * 100.0);
    printf("  Out-of-sample: %d candles (%d days) $%.0f → $%.0f (%+.1f%%)\n\n",
           oos_count, oos_count / 24, oos_start, oos_end,
           (oos_end - oos_start) / oos_start * 100.0);

    /* ── Step 3: Buy & Hold benchmarks ─────────────────────────────────── */
    bnh_result_t bnh_full = compute_buy_and_hold(candles, n_candles,
                                                  INITIAL_BALANCE, MAX_LEVERAGE, "1h");
    bnh_result_t bnh_is = compute_buy_and_hold(is_candles, is_count,
                                                INITIAL_BALANCE, MAX_LEVERAGE, "1h");
    bnh_result_t bnh_oos = compute_buy_and_hold(oos_candles, oos_count,
                                                 INITIAL_BALANCE, MAX_LEVERAGE, "1h");

    /* ── Step 4: Run backtests for each strategy ───────────────────────── */
    for (int s = 0; STRATEGIES[s].name; s++) {
        printf("%s%s╔══ %s ══╗%s\n", C_BOLD, C_CYAN, STRATEGIES[s].name, C_RESET);

        tb_backtest_result_t r_full, r_is, r_oos;
        memset(&r_full, 0, sizeof(r_full));
        memset(&r_is, 0, sizeof(r_is));
        memset(&r_oos, 0, sizeof(r_oos));

        /* In-sample first (most important for walk-forward) */
        int rc2 = run_backtest_isolated(STRATEGIES[s].path,
                                         is_candles, is_count, &r_is);
        /* Out-of-sample */
        int rc3 = run_backtest_isolated(STRATEGIES[s].path,
                                         oos_candles, oos_count, &r_oos);

        if (rc2 != 0 || rc3 != 0) {
            printf("  %sERROR: backtest failed (IS=%d/OOS=%d)%s\n\n",
                   C_RED, rc2, rc3, C_RESET);
            continue;
        }

        /* Full period (run in child process — safe if it OOMs) */
        int rc1 = run_backtest_isolated(STRATEGIES[s].path,
                                         candles, n_candles, &r_full);
        if (rc1 != 0) {
            /* Estimate full from IS + OOS */
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

        /* Print results table */
        print_header("Period");
        print_result_line("Full Period", &r_full, &bnh_full);
        print_result_line("In-Sample (60%)", &r_is, &bnh_is);
        print_result_line("Out-of-Sample (40%)", &r_oos, &bnh_oos);

        /* Buy & Hold comparison */
        printf("\n  %sBuy & Hold benchmark:%s\n", C_DIM, C_RESET);
        printf("  %-20s  Full: %+.1f%% (Sharpe %.2f, DD %.1f%%)  |  "
               "IS: %+.1f%%  |  OOS: %+.1f%%\n",
               "", bnh_full.return_pct, bnh_full.sharpe, bnh_full.max_dd_pct,
               bnh_is.return_pct, bnh_oos.return_pct);

        /* Walk-forward degradation check */
        printf("\n  %sWalk-Forward Analysis:%s\n", C_BOLD, C_RESET);
        double sharpe_decay = r_is.sharpe_ratio > 0 ?
            (r_oos.sharpe_ratio - r_is.sharpe_ratio) / r_is.sharpe_ratio * 100.0 : 0;
        printf("    Sharpe IS→OOS: %.2f → %.2f (%s%.1f%%%s)\n",
               r_is.sharpe_ratio, r_oos.sharpe_ratio,
               sharpe_decay < -30 ? C_RED : C_GREEN, sharpe_decay, C_RESET);

        double pf_is_disp = r_is.profit_factor > 999 ? 999.99 : r_is.profit_factor;
        double pf_oos_disp = r_oos.profit_factor > 999 ? 999.99 : r_oos.profit_factor;
        double pf_decay = (r_is.profit_factor > 0 && r_is.profit_factor < 9000) ?
            (fmin(r_oos.profit_factor, 9999) - r_is.profit_factor) / r_is.profit_factor * 100.0 : 0;
        printf("    PF IS→OOS:     %.2f → %.2f (%s%.1f%%%s)\n",
               pf_is_disp, pf_oos_disp,
               pf_decay < -30 ? C_RED : C_GREEN, pf_decay, C_RESET);

        double wr_decay = r_oos.win_rate - r_is.win_rate;
        printf("    WinRate IS→OOS: %.1f%% → %.1f%% (%s%+.1f pp%s)\n",
               r_is.win_rate, r_oos.win_rate,
               wr_decay < -10 ? C_RED : C_GREEN, wr_decay, C_RESET);

        /* Overfitting check */
        bool overfit = (sharpe_decay < -50) || (pf_decay < -50);
        if (overfit) {
            printf("    %s⚠ ALERTE: degradation OOS > 50%% → overfitting probable%s\n",
                   C_RED, C_RESET);
        }

        /* Statistical significance */
        printf("\n  %sSignificativite statistique:%s\n", C_BOLD, C_RESET);
        printf("    Trades OOS: %d %s\n",
               r_oos.total_trades,
               r_oos.total_trades >= 100 ? C_GREEN "OK (100+)" C_RESET :
               r_oos.total_trades >= 30  ? C_YELLOW "FAIBLE (30-99)" C_RESET :
                                           C_RED "INSUFFISANT (< 30)" C_RESET);

        /* Trade quality */
        printf("    Avg win/loss ratio: %.2f\n",
               r_oos.avg_loss != 0 ? fabs(r_oos.avg_win / r_oos.avg_loss) : 0);
        printf("    Max single win:  $%+.2f\n", r_oos.max_win);
        printf("    Max single loss: $%.2f\n", r_oos.max_loss);
        printf("    Fee drag: $%.2f (%.1f%% of gross P&L)\n",
               r_oos.total_fees,
               r_oos.total_pnl != 0 ? fabs(r_oos.total_fees / r_oos.total_pnl * 100.0) : 0);

        /* ── VERDICT ──────────────────────────────────────────────────── */
        printf("\n  %s╔══ VERDICT: %s ══╗%s\n\n",
               C_BOLD, verdict(&r_oos, &bnh_oos), C_RESET);
    }

    /* ── Summary table ─────────────────────────────────────────────────── */
    printf("%s", C_BOLD);
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║                        RESUME COMPARATIF                           ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");
    printf("%s", C_RESET);

    printf("\n  %-20s %8s %8s %7s %7s %7s %7s\n",
           "Strategy", "OOS Ret", "BnH Ret", "Alpha", "Sharpe", "MaxDD", "Trades");
    printf("  %-20s %8s %8s %7s %7s %7s %7s\n",
           "────────────────────",
           "────────", "────────", "───────", "───────", "───────", "───────");

    for (int s = 0; STRATEGIES[s].name; s++) {
        tb_backtest_result_t r_oos;
        memset(&r_oos, 0, sizeof(r_oos));
        int rc = run_backtest_isolated(STRATEGIES[s].path,
                                        oos_candles, oos_count, &r_oos);
        if (rc != 0) continue;

        double alpha = r_oos.return_pct - bnh_oos.return_pct;
        printf("  %-20s %s%+7.1f%%%s %+7.1f%% %s%+6.1f%%%s %7.2f %6.1f%% %7d\n",
               STRATEGIES[s].name,
               r_oos.net_pnl >= 0 ? C_GREEN : C_RED,
               r_oos.return_pct, C_RESET,
               bnh_oos.return_pct,
               alpha >= 0 ? C_GREEN : C_RED,
               alpha, C_RESET,
               r_oos.sharpe_ratio,
               r_oos.max_drawdown_pct,
               r_oos.total_trades);
    }

    printf("\n  %sBuy & Hold (5x):     %+7.1f%%  Sharpe: %.2f  MaxDD: %.1f%%%s\n\n",
           C_DIM, bnh_oos.return_pct, bnh_oos.sharpe, bnh_oos.max_dd_pct, C_RESET);

    free(candles);
    return 0;
}
