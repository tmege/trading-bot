#ifndef TB_REPORT_GEN_H
#define TB_REPORT_GEN_H

#include "core/config.h"
#include "data/macro_fetcher.h"
#include "data/fear_greed.h"
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct tb_report_gen tb_report_gen_t;

/* ── Trade summary for reports ─────────────────────────────────────────── */
typedef struct {
    char   coin[16];
    char   side[8];        /* "BUY" or "SELL" */
    double price;
    double size;
    double pnl;
    double fee;
    int64_t time_ms;
    char   strategy[64];
} tb_report_trade_t;

/* ── Per-strategy statistics ───────────────────────────────────────────── */
typedef struct {
    char   name[64];
    int    total_trades;
    int    winning_trades;
    int    losing_trades;
    double total_pnl;
    double total_fees;
    double max_win;
    double max_loss;
    double avg_win;
    double avg_loss;
} tb_strategy_stats_t;

/* ── Daily report data ─────────────────────────────────────────────────── */
typedef struct {
    /* Date */
    int    year, month, day;

    /* Account */
    double start_value;
    double end_value;
    double daily_pnl;
    double daily_fees;
    double daily_return_pct;

    /* Trades */
    tb_report_trade_t trades[256];
    int               n_trades;

    /* Per-strategy */
    tb_strategy_stats_t strategy_stats[8];
    int                 n_strategy_stats;

    /* Totals */
    int    total_trades;
    int    winning_trades;
    int    losing_trades;
    double win_rate;
    double profit_factor;   /* gross_profit / gross_loss */
    double max_drawdown;

    /* Market context */
    tb_macro_data_t     macro;
    tb_fear_greed_t     fear_greed;
} tb_daily_report_t;

/* ── Weekly report data ────────────────────────────────────────────────── */
typedef struct {
    int    year, week;

    /* Aggregated P&L */
    double total_pnl;
    double total_fees;
    double start_value;
    double end_value;
    double return_pct;

    /* Daily breakdown */
    tb_daily_report_t daily[7];
    int               n_days;

    /* Totals */
    int    total_trades;
    int    winning_trades;
    int    losing_trades;
    double win_rate;
    double profit_factor;
    double max_drawdown;
    double best_day_pnl;
    double worst_day_pnl;

    /* Per-strategy */
    tb_strategy_stats_t strategy_stats[8];
    int                 n_strategy_stats;
} tb_weekly_report_t;

/* ── API ───────────────────────────────────────────────────────────────── */

tb_report_gen_t *tb_report_gen_create(const tb_config_t *cfg, sqlite3 *db);
void             tb_report_gen_destroy(tb_report_gen_t *rg);

/* Generate daily report for a given date (YYYY-MM-DD). NULL date = today. */
int tb_report_gen_daily(tb_report_gen_t *rg, const char *date,
                        tb_daily_report_t *out);

/* Generate weekly report for the week containing the given date. */
int tb_report_gen_weekly(tb_report_gen_t *rg, const char *date,
                         tb_weekly_report_t *out);

/* Print daily report to stdout (ANSI-colored). */
void tb_report_print_daily(const tb_daily_report_t *r);

/* Print weekly report to stdout (ANSI-colored). */
void tb_report_print_weekly(const tb_weekly_report_t *r);

/* Write report to a file (plain text, no ANSI). */
int tb_report_write_daily(const tb_daily_report_t *r, const char *path);
int tb_report_write_weekly(const tb_weekly_report_t *r, const char *path);

#endif /* TB_REPORT_GEN_H */
