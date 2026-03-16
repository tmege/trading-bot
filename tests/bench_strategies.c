/*
 * Strategy Comparison Benchmark
 *
 * Runs all strategies through the backtest engine on identical data,
 * then prints a comparison table to identify the best performer.
 */

#include "core/types.h"
#include "backtest/backtest_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ── Generate synthetic candle data ────────────────────────────────────── */

/*
 * Generate realistic ETH-like 1h candles for N days.
 * Uses geometric brownian motion with mean reversion and volatility clustering.
 */
static int generate_candles(tb_candle_t *out, int max_candles,
                             double start_price, int n_days, uint32_t seed) {
    int n = n_days * 24;  /* 1h candles */
    if (n > max_candles) n = max_candles;

    /* Simple LCG RNG for reproducibility */
    uint32_t rng = seed;
    #define NEXT_RNG() (rng = rng * 1664525u + 1013904223u)
    #define RAND_NORM() ({ \
        double u1 = (double)(NEXT_RNG() & 0x7FFFFFFF) / 2147483647.0 + 1e-10; \
        double u2 = (double)(NEXT_RNG() & 0x7FFFFFFF) / 2147483647.0 + 1e-10; \
        sqrt(-2.0 * log(u1)) * cos(6.2831853 * u2); \
    })

    double price = start_price;
    double vol = 0.02;     /* hourly vol ~2% */
    double mean = start_price;
    int64_t ts = 1700000000000LL; /* arbitrary start time */

    for (int i = 0; i < n; i++) {
        /* Volatility clustering (GARCH-like) */
        double vol_shock = RAND_NORM() * 0.003;
        vol = fmax(0.005, fmin(0.06, vol + vol_shock));

        /* Mean reversion */
        double reversion = (mean - price) / mean * 0.01;

        /* Price movement */
        double ret = reversion + RAND_NORM() * vol;
        double new_price = price * (1.0 + ret);

        /* Build candle */
        double open = price;
        double close = new_price;
        double high = fmax(open, close) * (1.0 + fabs(RAND_NORM()) * vol * 0.3);
        double low = fmin(open, close) * (1.0 - fabs(RAND_NORM()) * vol * 0.3);
        double volume = 1000.0 + fabs(RAND_NORM()) * 5000.0;

        /* Occasional volume spikes */
        if ((NEXT_RNG() % 100) < 5) volume *= 3.0;

        out[i].time_open = ts + (int64_t)i * 3600000LL;
        out[i].time_close = out[i].time_open + 3599999LL;
        out[i].open  = tb_decimal_from_double(open, 2);
        out[i].high  = tb_decimal_from_double(high, 2);
        out[i].low   = tb_decimal_from_double(low, 2);
        out[i].close = tb_decimal_from_double(close, 2);
        out[i].volume = tb_decimal_from_double(volume, 2);

        price = new_price;
    }

    #undef NEXT_RNG
    #undef RAND_NORM
    return n;
}

/* ── Strategy definitions ──────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *path;
    const char *description;
} strategy_entry_t;

static const strategy_entry_t strategies[] = {
    { "Regime Adaptive", "strategies/regime_adaptive_1h.lua",    "ADX regime detection, ATR SL/TP, OBV confirm" },
};

#define N_STRATEGIES (sizeof(strategies) / sizeof(strategies[0]))

/* ── Market scenarios ──────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    double      start_price;
    uint32_t    seed;
    int         n_days;
} scenario_t;

static const scenario_t scenarios[] = {
    { "Ranging (30d)",    2000.0,  42,     30 },
    { "Bull trend (30d)", 1800.0,  12345,  30 },
    { "Bear trend (30d)", 2200.0,  99999,  30 },
    { "High vol (30d)",   2000.0,  77777,  30 },
    { "Long run (90d)",   2000.0,  54321,  90 },
};

#define N_SCENARIOS (sizeof(scenarios) / sizeof(scenarios[0]))

/* ── Run one backtest ──────────────────────────────────────────────────── */

static int run_backtest(const strategy_entry_t *strat, const scenario_t *scen,
                         tb_backtest_result_t *result) {
    tb_backtest_config_t cfg = {
        .coin            = "ETH",
        .strategy_path   = strat->path,
        .initial_balance = 100.0,
        .max_leverage    = 5,
        .maker_fee_rate  = 0.0002,
        .taker_fee_rate  = 0.0005,
        .slippage_bps    = 1.0,
    };

    tb_backtest_engine_t *bt = tb_backtest_create(&cfg);
    if (!bt) return -1;

    /* Generate candles */
    int max_candles = scen->n_days * 24;
    tb_candle_t *candles = calloc((size_t)max_candles, sizeof(tb_candle_t));
    if (!candles) {
        tb_backtest_destroy(bt);
        return -1;
    }

    int n = generate_candles(candles, max_candles, scen->start_price,
                              scen->n_days, scen->seed);

    int rc = tb_backtest_load_candles(bt, candles, n);
    if (rc != 0) {
        free(candles);
        tb_backtest_destroy(bt);
        return -1;
    }

    rc = tb_backtest_run(bt, result);

    free(candles);
    tb_backtest_destroy(bt);
    return rc;
}

/* ── Print comparison table ────────────────────────────────────────────── */

static void print_separator(void) {
    printf("+");
    for (int i = 0; i < 156; i++) printf("-");
    printf("+\n");
}

static const char *color_pnl(double v) {
    return v >= 0 ? "\033[32m" : "\033[31m";
}

static void print_scenario_results(const scenario_t *scen,
                                    tb_backtest_result_t results[],
                                    int n_strats) {
    printf("\n\033[1;36m=== %s (start=$%.0f, %d days) ===\033[0m\n\n",
           scen->name, scen->start_price, scen->n_days);

    printf("  %-16s %8s %8s %7s %7s %7s %8s %8s %7s %6s\n",
           "Strategy", "P&L", "Return", "Sharpe", "Sortino", "MaxDD",
           "Trades", "WinRate", "PF", "Fees");
    printf("  %-16s %8s %8s %7s %7s %7s %8s %8s %7s %6s\n",
           "────────────────", "────────", "────────", "───────", "───────", "───────",
           "────────", "────────", "───────", "──────");

    /* Find best P&L for highlighting */
    double best_pnl = -1e9;
    int best_idx = -1;
    for (int i = 0; i < n_strats; i++) {
        if (results[i].net_pnl > best_pnl) {
            best_pnl = results[i].net_pnl;
            best_idx = i;
        }
    }

    for (int i = 0; i < n_strats; i++) {
        tb_backtest_result_t *r = &results[i];
        const char *marker = (i == best_idx) ? " \033[1;33m★\033[0m" : "  ";

        printf("%s %-16s %s%+8.2f\033[0m %s%+7.1f%%\033[0m %7.2f %7.2f %6.1f%% %8d %7.1f%% %7.2f %5.2f\n",
               marker,
               strategies[i].name,
               color_pnl(r->net_pnl), r->net_pnl,
               color_pnl(r->return_pct), r->return_pct,
               r->sharpe_ratio,
               r->sortino_ratio,
               r->max_drawdown_pct,
               r->total_trades,
               r->win_rate,
               r->profit_factor,
               r->total_fees);
    }
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("\n\033[1;37m");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║          STRATEGY COMPARISON BENCHMARK                     ║\n");
    printf("║  5 strategies × 5 scenarios = 25 backtests                 ║\n");
    printf("║  Initial balance: 100 USDC, 5x leverage                   ║\n");
    printf("║  Fees: maker 2bps, taker 5bps, slippage 1bp               ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("\033[0m\n");

    /* Aggregate scores */
    double total_pnl[N_STRATEGIES] = {0};
    double total_sharpe[N_STRATEGIES] = {0};
    double total_dd[N_STRATEGIES] = {0};
    int    total_wins[N_STRATEGIES] = {0};

    for (int s = 0; s < (int)N_SCENARIOS; s++) {
        tb_backtest_result_t results[N_STRATEGIES];
        memset(results, 0, sizeof(results));

        printf("\033[2mRunning scenario: %s...\033[0m\n", scenarios[s].name);

        for (int i = 0; i < (int)N_STRATEGIES; i++) {
            int rc = run_backtest(&strategies[i], &scenarios[s], &results[i]);
            if (rc != 0) {
                printf("  \033[31mFAILED: %s\033[0m\n", strategies[i].name);
                continue;
            }
        }

        print_scenario_results(&scenarios[s], results, (int)N_STRATEGIES);

        /* Accumulate for overall ranking */
        double best_this = -1e9;
        int best_this_idx = -1;
        for (int i = 0; i < (int)N_STRATEGIES; i++) {
            total_pnl[i] += results[i].net_pnl;
            total_sharpe[i] += results[i].sharpe_ratio;
            total_dd[i] += results[i].max_drawdown_pct;
            if (results[i].net_pnl > best_this) {
                best_this = results[i].net_pnl;
                best_this_idx = i;
            }
        }
        if (best_this_idx >= 0) total_wins[best_this_idx]++;
    }

    /* ── Overall ranking ───────────────────────────────────────────────── */
    printf("\n\033[1;37m");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                    OVERALL RANKING                         ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("\033[0m\n");

    printf("  %-16s %10s %10s %10s %6s\n",
           "Strategy", "Total P&L", "Avg Sharpe", "Avg MaxDD", "Wins");
    printf("  %-16s %10s %10s %10s %6s\n",
           "────────────────", "──────────", "──────────", "──────────", "──────");

    /* Sort by total P&L */
    int order[N_STRATEGIES];
    for (int i = 0; i < (int)N_STRATEGIES; i++) order[i] = i;
    for (int i = 0; i < (int)N_STRATEGIES - 1; i++) {
        for (int j = i + 1; j < (int)N_STRATEGIES; j++) {
            if (total_pnl[order[j]] > total_pnl[order[i]]) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
        }
    }

    const char *medals[] = { "\033[1;33m🥇", "\033[37m🥈", "\033[33m🥉", "  ", "  " };
    for (int rank = 0; rank < (int)N_STRATEGIES; rank++) {
        int i = order[rank];
        double avg_sharpe = total_sharpe[i] / (double)(int)N_SCENARIOS;
        double avg_dd = total_dd[i] / (double)(int)N_SCENARIOS;

        printf(" %s\033[0m %-16s %s%+10.2f\033[0m %10.2f %9.1f%% %5d/%d\n",
               medals[rank],
               strategies[i].name,
               color_pnl(total_pnl[i]), total_pnl[i],
               avg_sharpe,
               avg_dd,
               total_wins[i], (int)N_SCENARIOS);
    }

    printf("\n\033[2m  Recommendation: Use the top-ranked strategy as primary,\n");
    printf("  second-ranked as secondary. Diversification across uncorrelated\n");
    printf("  strategies reduces overall portfolio risk.\033[0m\n\n");

    return 0;
}
