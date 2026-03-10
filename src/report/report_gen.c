#include "report/report_gen.h"
#include "core/logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── ANSI color codes ───────────────────────────────────────────────────── */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_BLUE    "\033[34m"
#define C_CYAN    "\033[36m"
#define C_WHITE   "\033[37m"

#define LINE_W 72

struct tb_report_gen {
    const tb_config_t *cfg;
    sqlite3           *db;
};

/* ── Helpers ───────────────────────────────────────────────────────────── */

static void rprint_line(FILE *f, bool color) {
    if (color) fprintf(f, C_DIM);
    for (int i = 0; i < LINE_W; i++) fprintf(f, "─");
    if (color) fprintf(f, C_RESET);
    fprintf(f, "\n");
}

static void rprint_header(FILE *f, const char *title, bool color) {
    if (color)
        fprintf(f, C_BOLD C_CYAN "  %-*s" C_RESET "\n", LINE_W - 2, title);
    else
        fprintf(f, "  %-*s\n", LINE_W - 2, title);
    rprint_line(f, color);
}

static const char *pnl_clr(double v, bool color) {
    if (!color) return "";
    return v >= 0 ? C_GREEN : C_RED;
}

static const char *rst(bool color) {
    return color ? C_RESET : "";
}

/* ── Load trades from SQLite ───────────────────────────────────────────── */
static int load_trades_for_date(sqlite3 *db, const char *date,
                                tb_report_trade_t *out, int max_trades) {
    if (!db) return 0;

    const char *sql =
        "SELECT coin, side, price, size, pnl, fee, timestamp_ms, strategy "
        "FROM trades WHERE date(timestamp_ms/1000, 'unixepoch') = ? "
        "ORDER BY timestamp_ms ASC LIMIT ?";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        tb_log_warn("report: failed to prepare trades query: %s",
                    sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, date, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, max_trades);

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < max_trades) {
        tb_report_trade_t *t = &out[n];
        const char *coin = (const char *)sqlite3_column_text(stmt, 0);
        const char *side = (const char *)sqlite3_column_text(stmt, 1);
        if (coin) strncpy(t->coin, coin, sizeof(t->coin) - 1);
        if (side) strncpy(t->side, side, sizeof(t->side) - 1);
        t->price   = sqlite3_column_double(stmt, 2);
        t->size    = sqlite3_column_double(stmt, 3);
        t->pnl     = sqlite3_column_double(stmt, 4);
        t->fee     = sqlite3_column_double(stmt, 5);
        t->time_ms = sqlite3_column_int64(stmt, 6);
        const char *strat = (const char *)sqlite3_column_text(stmt, 7);
        if (strat) strncpy(t->strategy, strat, sizeof(t->strategy) - 1);
        n++;
    }

    sqlite3_finalize(stmt);
    return n;
}

static double load_account_value_at(sqlite3 *db, const char *date) {
    if (!db) return 0;

    /* daily_pnl has realized_pnl, unrealized_pnl, fees_paid — no account_value.
       Approximate: sum realized_pnl for the date as a proxy. */
    const char *sql =
        "SELECT CAST(realized_pnl AS REAL) FROM daily_pnl WHERE date = ? LIMIT 1";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, date, -1, SQLITE_STATIC);

    double val = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        val = sqlite3_column_double(stmt, 0);

    sqlite3_finalize(stmt);
    return val;
}

/* ── Compute strategy stats from trades ────────────────────────────────── */
static void compute_strategy_stats(const tb_report_trade_t *trades, int n,
                                   tb_strategy_stats_t *stats, int *n_stats) {
    *n_stats = 0;

    for (int i = 0; i < n; i++) {
        /* Find or create strategy entry */
        int idx = -1;
        for (int j = 0; j < *n_stats; j++) {
            if (strcmp(stats[j].name, trades[i].strategy) == 0) {
                idx = j;
                break;
            }
        }
        if (idx < 0) {
            if (*n_stats >= 8) continue;
            idx = (*n_stats)++;
            memset(&stats[idx], 0, sizeof(tb_strategy_stats_t));
            strncpy(stats[idx].name, trades[i].strategy,
                    sizeof(stats[idx].name) - 1);
        }

        tb_strategy_stats_t *s = &stats[idx];
        s->total_trades++;
        s->total_pnl += trades[i].pnl;
        s->total_fees += trades[i].fee;

        if (trades[i].pnl > 0) {
            s->winning_trades++;
            if (trades[i].pnl > s->max_win) s->max_win = trades[i].pnl;
        } else if (trades[i].pnl < 0) {
            s->losing_trades++;
            if (trades[i].pnl < s->max_loss) s->max_loss = trades[i].pnl;
        }
    }

    /* Compute averages */
    for (int i = 0; i < *n_stats; i++) {
        if (stats[i].winning_trades > 0)
            stats[i].avg_win = stats[i].total_pnl > 0 ?
                stats[i].total_pnl / stats[i].winning_trades : 0;
        if (stats[i].losing_trades > 0)
            stats[i].avg_loss = stats[i].total_pnl < 0 ?
                stats[i].total_pnl / stats[i].losing_trades : 0;
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

tb_report_gen_t *tb_report_gen_create(const tb_config_t *cfg, sqlite3 *db) {
    tb_report_gen_t *rg = calloc(1, sizeof(tb_report_gen_t));
    if (!rg) return NULL;
    rg->cfg = cfg;
    rg->db  = db;
    return rg;
}

void tb_report_gen_destroy(tb_report_gen_t *rg) {
    free(rg);
}

int tb_report_gen_daily(tb_report_gen_t *rg, const char *date,
                        tb_daily_report_t *out) {
    memset(out, 0, sizeof(*out));

    /* Resolve date */
    char date_buf[16];
    if (date) {
        strncpy(date_buf, date, sizeof(date_buf) - 1);
    } else {
        time_t now = time(NULL);
        struct tm utc;
        gmtime_r(&now, &utc);
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &utc);
    }

    sscanf(date_buf, "%d-%d-%d", &out->year, &out->month, &out->day);

    /* Load trades */
    out->n_trades = load_trades_for_date(rg->db, date_buf,
                                         out->trades, 256);

    /* Load account value */
    out->start_value = load_account_value_at(rg->db, date_buf);
    out->end_value = out->start_value; /* updated below */

    /* Compute totals from trades */
    double gross_profit = 0, gross_loss = 0;
    for (int i = 0; i < out->n_trades; i++) {
        out->daily_pnl  += out->trades[i].pnl;
        out->daily_fees += out->trades[i].fee;

        if (out->trades[i].pnl > 0) {
            out->winning_trades++;
            gross_profit += out->trades[i].pnl;
        } else if (out->trades[i].pnl < 0) {
            out->losing_trades++;
            gross_loss += -out->trades[i].pnl;
        }
    }

    out->total_trades = out->n_trades;
    out->end_value = out->start_value + out->daily_pnl - out->daily_fees;

    if (out->total_trades > 0)
        out->win_rate = (double)out->winning_trades / out->total_trades * 100.0;

    if (gross_loss > 0)
        out->profit_factor = gross_profit / gross_loss;

    if (out->start_value > 0)
        out->daily_return_pct = (out->daily_pnl - out->daily_fees) /
                                 out->start_value * 100.0;

    /* Drawdown (running min of cumulative P&L) */
    double cum_pnl = 0, peak = 0;
    for (int i = 0; i < out->n_trades; i++) {
        cum_pnl += out->trades[i].pnl - out->trades[i].fee;
        if (cum_pnl > peak) peak = cum_pnl;
        double dd = peak - cum_pnl;
        if (dd > out->max_drawdown) out->max_drawdown = dd;
    }

    /* Strategy stats */
    compute_strategy_stats(out->trades, out->n_trades,
                           out->strategy_stats, &out->n_strategy_stats);

    return 0;
}

int tb_report_gen_weekly(tb_report_gen_t *rg, const char *date,
                         tb_weekly_report_t *out) {
    memset(out, 0, sizeof(*out));

    /* Resolve date and find Monday of that week */
    time_t base;
    if (date) {
        struct tm tm_date = {0};
        sscanf(date, "%d-%d-%d", &tm_date.tm_year, &tm_date.tm_mon,
               &tm_date.tm_mday);
        tm_date.tm_year -= 1900;
        tm_date.tm_mon -= 1;
        base = timegm(&tm_date);
    } else {
        base = time(NULL);
    }

    struct tm utc;
    gmtime_r(&base, &utc);
    /* Rewind to Monday (tm_wday: 0=Sun, 1=Mon) */
    int days_since_monday = (utc.tm_wday + 6) % 7;
    time_t monday = base - days_since_monday * 86400;

    struct tm monday_tm;
    gmtime_r(&monday, &monday_tm);
    out->year = monday_tm.tm_year + 1900;
    /* ISO week number */
    char week_str[8];
    strftime(week_str, sizeof(week_str), "%V", &monday_tm);
    out->week = atoi(week_str);

    /* Generate daily reports for each day of the week */
    double gross_profit = 0, gross_loss = 0;
    out->best_day_pnl = -1e18;
    out->worst_day_pnl = 1e18;

    for (int d = 0; d < 7; d++) {
        time_t day_t = monday + d * 86400;
        struct tm day_tm;
        gmtime_r(&day_t, &day_tm);

        /* Skip future days */
        if (day_t > time(NULL)) break;

        char day_str[16];
        strftime(day_str, sizeof(day_str), "%Y-%m-%d", &day_tm);

        tb_daily_report_t *daily = &out->daily[out->n_days];
        tb_report_gen_daily(rg, day_str, daily);
        out->n_days++;

        /* Aggregate */
        out->total_pnl    += daily->daily_pnl;
        out->total_fees   += daily->daily_fees;
        out->total_trades += daily->total_trades;
        out->winning_trades += daily->winning_trades;
        out->losing_trades  += daily->losing_trades;

        double net = daily->daily_pnl - daily->daily_fees;
        if (net > out->best_day_pnl)  out->best_day_pnl = net;
        if (net < out->worst_day_pnl) out->worst_day_pnl = net;

        for (int i = 0; i < daily->n_trades; i++) {
            if (daily->trades[i].pnl > 0) gross_profit += daily->trades[i].pnl;
            else if (daily->trades[i].pnl < 0) gross_loss += -daily->trades[i].pnl;
        }
    }

    if (out->n_days == 0) {
        out->best_day_pnl = 0;
        out->worst_day_pnl = 0;
    }

    /* Account values from first/last day */
    if (out->n_days > 0) {
        out->start_value = out->daily[0].start_value;
        out->end_value   = out->daily[out->n_days - 1].end_value;
    }

    if (out->start_value > 0)
        out->return_pct = (out->total_pnl - out->total_fees) /
                           out->start_value * 100.0;

    if (out->total_trades > 0)
        out->win_rate = (double)out->winning_trades / out->total_trades * 100.0;

    if (gross_loss > 0)
        out->profit_factor = gross_profit / gross_loss;

    /* Weekly drawdown */
    double cum = 0, peak = 0;
    for (int d = 0; d < out->n_days; d++) {
        double net = out->daily[d].daily_pnl - out->daily[d].daily_fees;
        cum += net;
        if (cum > peak) peak = cum;
        double dd = peak - cum;
        if (dd > out->max_drawdown) out->max_drawdown = dd;
    }

    /* Aggregate strategy stats from all daily trades */
    tb_report_trade_t *all_trades = malloc(sizeof(tb_report_trade_t) * 1792);
    int all_n = 0;
    if (all_trades) {
        for (int d = 0; d < out->n_days; d++) {
            for (int i = 0; i < out->daily[d].n_trades && all_n < 1792; i++) {
                all_trades[all_n++] = out->daily[d].trades[i];
            }
        }
        compute_strategy_stats(all_trades, all_n,
                               out->strategy_stats, &out->n_strategy_stats);
        free(all_trades);
    }

    return 0;
}

/* ── Print helpers ─────────────────────────────────────────────────────── */

static void print_daily_impl(FILE *f, const tb_daily_report_t *r, bool color) {
    fprintf(f, "\n");
    if (color)
        fprintf(f, C_BOLD C_WHITE "  📊 DAILY REPORT — %04d-%02d-%02d" C_RESET "\n",
                r->year, r->month, r->day);
    else
        fprintf(f, "  DAILY REPORT — %04d-%02d-%02d\n",
                r->year, r->month, r->day);
    rprint_line(f, color);

    /* Summary */
    rprint_header(f, "SUMMARY", color);
    fprintf(f, "  P&L:    %s%+.4f USDC%s    Fees: %s-%.4f USDC%s    Net: %s%+.4f USDC%s\n",
            pnl_clr(r->daily_pnl, color), r->daily_pnl, rst(color),
            color ? C_RED : "", r->daily_fees, rst(color),
            pnl_clr(r->daily_pnl - r->daily_fees, color),
            r->daily_pnl - r->daily_fees, rst(color));
    fprintf(f, "  Trades: %d (W:%d L:%d)  Win rate: %.1f%%  PF: %.2f\n",
            r->total_trades, r->winning_trades, r->losing_trades,
            r->win_rate, r->profit_factor);
    if (r->start_value > 0)
        fprintf(f, "  Account: %.2f USDC → %.2f USDC  Return: %s%+.2f%%%s\n",
                r->start_value, r->end_value,
                pnl_clr(r->daily_return_pct, color),
                r->daily_return_pct, rst(color));
    if (r->max_drawdown > 0)
        fprintf(f, "  Max drawdown: %s-%.4f USDC%s\n",
                color ? C_RED : "", r->max_drawdown, rst(color));

    /* Strategy breakdown */
    if (r->n_strategy_stats > 0) {
        rprint_header(f, "BY STRATEGY", color);
        for (int i = 0; i < r->n_strategy_stats; i++) {
            const tb_strategy_stats_t *s = &r->strategy_stats[i];
            double wr = s->total_trades > 0 ?
                (double)s->winning_trades / s->total_trades * 100.0 : 0;
            fprintf(f, "  %-20s  %s%+.4f USDC%s  %d trades  WR %.0f%%\n",
                    s->name, pnl_clr(s->total_pnl, color),
                    s->total_pnl, rst(color), s->total_trades, wr);
        }
    }

    /* Trade list */
    if (r->n_trades > 0) {
        rprint_header(f, "TRADES", color);
        if (color)
            fprintf(f, C_DIM "  %-8s %-6s %-5s %10s %10s %10s %10s" C_RESET "\n",
                    "TIME", "COIN", "SIDE", "PRICE", "SIZE", "PnL", "FEE");
        else
            fprintf(f, "  %-8s %-6s %-5s %10s %10s %10s %10s\n",
                    "TIME", "COIN", "SIDE", "PRICE", "SIZE", "PnL", "FEE");

        int shown = 0;
        for (int i = 0; i < r->n_trades && shown < 50; i++) {
            const tb_report_trade_t *t = &r->trades[i];
            time_t ts = (time_t)(t->time_ms / 1000);
            struct tm tm_t;
            gmtime_r(&ts, &tm_t);
            char ts_str[16];
            strftime(ts_str, sizeof(ts_str), "%H:%M:%S", &tm_t);

            const char *side_clr = "";
            if (color) side_clr = strcmp(t->side, "BUY") == 0 ? C_GREEN : C_RED;

            fprintf(f, "  %-8s %-6s %s%-5s%s %10.2f %10.4f %s%+10.4f%s %10.4f\n",
                    ts_str, t->coin, side_clr, t->side, rst(color),
                    t->price, t->size,
                    pnl_clr(t->pnl, color), t->pnl, rst(color),
                    t->fee);
            shown++;
        }
        if (r->n_trades > 50)
            fprintf(f, "  ... +%d more trades\n", r->n_trades - 50);
    }

    rprint_line(f, color);
    fprintf(f, "\n");
}

static void print_weekly_impl(FILE *f, const tb_weekly_report_t *r, bool color) {
    fprintf(f, "\n");
    if (color)
        fprintf(f, C_BOLD C_WHITE "  📈 WEEKLY REPORT — %04d W%02d" C_RESET "\n",
                r->year, r->week);
    else
        fprintf(f, "  WEEKLY REPORT — %04d W%02d\n", r->year, r->week);
    rprint_line(f, color);

    /* Summary */
    rprint_header(f, "WEEKLY SUMMARY", color);
    fprintf(f, "  P&L:    %s%+.4f USDC%s    Fees: %s-%.4f USDC%s    Net: %s%+.4f USDC%s\n",
            pnl_clr(r->total_pnl, color), r->total_pnl, rst(color),
            color ? C_RED : "", r->total_fees, rst(color),
            pnl_clr(r->total_pnl - r->total_fees, color),
            r->total_pnl - r->total_fees, rst(color));
    fprintf(f, "  Trades: %d (W:%d L:%d)  Win rate: %.1f%%  PF: %.2f\n",
            r->total_trades, r->winning_trades, r->losing_trades,
            r->win_rate, r->profit_factor);
    if (r->start_value > 0)
        fprintf(f, "  Account: %.2f USDC → %.2f USDC  Return: %s%+.2f%%%s\n",
                r->start_value, r->end_value,
                pnl_clr(r->return_pct, color),
                r->return_pct, rst(color));
    fprintf(f, "  Best day: %s%+.4f USDC%s  Worst: %s%+.4f USDC%s  Max DD: %s-%.4f USDC%s\n",
            pnl_clr(r->best_day_pnl, color), r->best_day_pnl, rst(color),
            pnl_clr(r->worst_day_pnl, color), r->worst_day_pnl, rst(color),
            color ? C_RED : "", r->max_drawdown, rst(color));

    /* Daily breakdown */
    rprint_header(f, "DAILY BREAKDOWN", color);
    if (color)
        fprintf(f, C_DIM "  %-12s %8s %6s %8s %8s" C_RESET "\n",
                "DATE", "P&L", "TRADES", "WIN%", "NET");
    else
        fprintf(f, "  %-12s %8s %6s %8s %8s\n",
                "DATE", "P&L", "TRADES", "WIN%", "NET");

    for (int i = 0; i < r->n_days; i++) {
        const tb_daily_report_t *d = &r->daily[i];
        double net = d->daily_pnl - d->daily_fees;
        fprintf(f, "  %04d-%02d-%02d   %s%+8.4f%s %6d %7.1f%% %s%+8.4f%s\n",
                d->year, d->month, d->day,
                pnl_clr(d->daily_pnl, color), d->daily_pnl, rst(color),
                d->total_trades, d->win_rate,
                pnl_clr(net, color), net, rst(color));
    }

    /* Strategy breakdown */
    if (r->n_strategy_stats > 0) {
        rprint_header(f, "BY STRATEGY", color);
        for (int i = 0; i < r->n_strategy_stats; i++) {
            const tb_strategy_stats_t *s = &r->strategy_stats[i];
            double wr = s->total_trades > 0 ?
                (double)s->winning_trades / s->total_trades * 100.0 : 0;
            fprintf(f, "  %-20s  %s%+.4f USDC%s  %d trades  WR %.0f%%"
                    "  best %s%+.4f%s  worst %s%+.4f%s\n",
                    s->name, pnl_clr(s->total_pnl, color),
                    s->total_pnl, rst(color), s->total_trades, wr,
                    pnl_clr(s->max_win, color), s->max_win, rst(color),
                    pnl_clr(s->max_loss, color), s->max_loss, rst(color));
        }
    }

    rprint_line(f, color);
    fprintf(f, "\n");
}

/* ── Public print/write functions ──────────────────────────────────────── */

void tb_report_print_daily(const tb_daily_report_t *r) {
    print_daily_impl(stdout, r, true);
}

void tb_report_print_weekly(const tb_weekly_report_t *r) {
    print_weekly_impl(stdout, r, true);
}

int tb_report_write_daily(const tb_daily_report_t *r, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        tb_log_warn("report: cannot write to %s", path);
        return -1;
    }
    print_daily_impl(f, r, false);
    fclose(f);
    tb_log_info("report: daily report written to %s", path);
    return 0;
}

int tb_report_write_weekly(const tb_weekly_report_t *r, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        tb_log_warn("report: cannot write to %s", path);
        return -1;
    }
    print_weekly_impl(f, r, false);
    fclose(f);
    tb_log_info("report: weekly report written to %s", path);
    return 0;
}
