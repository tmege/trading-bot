#ifndef TB_BACKTEST_ENGINE_H
#define TB_BACKTEST_ENGINE_H

#include "core/types.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Backtest configuration ────────────────────────────────────────────── */
typedef struct {
    const char *coin;              /* e.g. "ETH" */
    const char *strategy_path;     /* path to .lua file */
    double      initial_balance;   /* starting account value */
    int         max_leverage;
    double      maker_fee_rate;    /* e.g. 0.0002 (2 bps) */
    double      taker_fee_rate;    /* e.g. 0.0005 (5 bps) */
    double      slippage_bps;      /* simulated slippage in basis points */
} tb_backtest_config_t;

/* ── Per-trade log entry ───────────────────────────────────────────────── */
typedef struct {
    int64_t time_ms;
    char    side[8];
    double  price;
    double  size;
    double  pnl;
    double  fee;
    double  balance_after;
} tb_bt_trade_t;

/* ── Backtest results ──────────────────────────────────────────────────── */
typedef struct {
    /* P&L */
    double start_balance;
    double end_balance;
    double total_pnl;
    double total_fees;
    double net_pnl;
    double return_pct;

    /* Trade stats */
    int    total_trades;
    int    winning_trades;
    int    losing_trades;
    double win_rate;
    double profit_factor;
    double avg_win;
    double avg_loss;
    double max_win;
    double max_loss;

    /* Risk metrics */
    double max_drawdown;
    double max_drawdown_pct;
    double sharpe_ratio;       /* annualized, assuming 365 days */
    double sortino_ratio;

    /* Time */
    int64_t start_time_ms;
    int64_t end_time_ms;
    int     n_candles;
    int     n_days;

    /* Per-trade log (dynamically allocated) */
    tb_bt_trade_t *trades;
    int n_trade_log;
    int trade_cap;

    /* Equity curve (sampled daily) */
    struct {
        int64_t time_ms;
        double  equity;
        double  drawdown;
    } equity_curve[2000];
    int n_equity_points;
} tb_backtest_result_t;

typedef struct tb_backtest_engine tb_backtest_engine_t;

/* ── API ───────────────────────────────────────────────────────────────── */

tb_backtest_engine_t *tb_backtest_create(const tb_backtest_config_t *cfg);
void                  tb_backtest_destroy(tb_backtest_engine_t *bt);

/* Load historical candles for backtesting.
   interval: "1h", "15m", "1d", etc. */
int tb_backtest_load_candles(tb_backtest_engine_t *bt,
                             const tb_candle_t *candles, int n_candles);

/* Run the backtest. Returns 0 on success. */
int tb_backtest_run(tb_backtest_engine_t *bt, tb_backtest_result_t *out);

/* Print results to stdout. */
void tb_backtest_print_results(const tb_backtest_result_t *r);

/* Free dynamically allocated trade log. Safe to call on zeroed struct. */
void tb_backtest_result_cleanup(tb_backtest_result_t *r);

#endif /* TB_BACKTEST_ENGINE_H */
