#include "report/dashboard.h"
#include "core/logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

struct tb_dashboard {
    tb_dashboard_data_t data;
    pthread_mutex_t     lock;
    pthread_t           thread;
    bool                running;
    bool                started;
    int                 refresh_ms;
};

/* ── ANSI color codes ───────────────────────────────────────────────────── */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_BLUE    "\033[34m"
#define C_MAGENTA "\033[35m"
#define C_CYAN    "\033[36m"
#define C_WHITE   "\033[37m"
#define C_BG_RED  "\033[41m"
#define C_BG_GRN  "\033[42m"

#define LINE_W 72

static void print_line(void) {
    printf(C_DIM);
    for (int i = 0; i < LINE_W; i++) printf("─");
    printf(C_RESET "\n");
}

static void print_header(const char *title) {
    printf(C_BOLD C_CYAN "  %-*s" C_RESET "\n", LINE_W - 2, title);
    print_line();
}

static const char *pnl_color(double v) {
    return v >= 0 ? C_GREEN : C_RED;
}

static void format_time(int64_t sec, char *buf, size_t len) {
    int d = (int)(sec / 86400);
    int h = (int)((sec % 86400) / 3600);
    int m = (int)((sec % 3600) / 60);
    if (d > 0)
        snprintf(buf, len, "%dd %dh %dm", d, h, m);
    else if (h > 0)
        snprintf(buf, len, "%dh %dm", h, m);
    else
        snprintf(buf, len, "%dm", m);
}

/* ── Main render function ───────────────────────────────────────────────── */
void tb_dashboard_render(const tb_dashboard_data_t *d) {
    /* Clear screen */
    printf("\033[2J\033[H");

    /* Header */
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S UTC", &utc);

    char uptime[32];
    format_time(d->uptime_sec, uptime, sizeof(uptime));

    printf(C_BOLD C_WHITE "\n  ⚡ TRADING BOT");
    if (d->paper_mode) printf(C_YELLOW " [PAPER]");
    printf(C_DIM "  %s  up %s" C_RESET "\n", time_str, uptime);
    print_line();

    /* ── Account ──────────────────────────────────────────────────────── */
    print_header("ACCOUNT");
    printf("  Value:  " C_BOLD "$%.2f" C_RESET, d->account_value);
    printf("    Daily P&L: %s%.2f USDC" C_RESET, pnl_color(d->daily_pnl), d->daily_pnl);
    printf("    Fees: " C_RED "-%.4f USDC" C_RESET, d->daily_fees);
    printf("    Trades: %d\n", d->daily_trades);

    /* ── Positions ────────────────────────────────────────────────────── */
    if (d->n_positions > 0) {
        print_header("POSITIONS");
        printf(C_DIM "  %-6s %8s %10s %10s %6s %10s" C_RESET "\n",
               "COIN", "SIZE", "ENTRY", "MARK", "LEV", "uPnL");
        for (int i = 0; i < d->n_positions; i++) {
            const tb_position_t *p = &d->positions[i];
            double sz = tb_decimal_to_double(p->size);
            if (sz == 0) continue;
            double entry = tb_decimal_to_double(p->entry_px);
            double upnl = tb_decimal_to_double(p->unrealized_pnl);

            printf("  %-6s %s%8.4f" C_RESET " %10.2f %10s %4dx  %s%+.4f" C_RESET "\n",
                   p->coin,
                   sz > 0 ? C_GREEN : C_RED, sz,
                   entry,
                   "—",
                   p->leverage,
                   pnl_color(upnl), upnl);
        }
    }

    /* ── Open Orders ──────────────────────────────────────────────────── */
    if (d->n_orders > 0) {
        print_header("OPEN ORDERS");
        printf(C_DIM "  %-6s %5s %10s %10s" C_RESET "\n",
               "COIN", "SIDE", "PRICE", "SIZE");
        int shown = 0;
        for (int i = 0; i < d->n_orders && shown < 20; i++) {
            const tb_order_t *o = &d->orders[i];
            printf("  %-6s %s%5s" C_RESET " %10.2f %10.4f\n",
                   o->coin,
                   o->side == TB_SIDE_BUY ? C_GREEN : C_RED,
                   o->side == TB_SIDE_BUY ? "BUY" : "SELL",
                   tb_decimal_to_double(o->limit_px),
                   tb_decimal_to_double(o->sz));
            shown++;
        }
        if (d->n_orders > 20) {
            printf(C_DIM "  ... +%d more orders" C_RESET "\n", d->n_orders - 20);
        }
    }

    /* ── Market Data ──────────────────────────────────────────────────── */
    print_header("MARKET");
    if (d->macro.valid) {
        printf("  BTC " C_BOLD "$%.0f" C_RESET "  dom %.1f%%  ETH/BTC %.5f  T2 $%.0fB\n",
               d->macro.btc_price, d->macro.btc_dominance,
               d->macro.eth_btc, d->macro.total2_mcap);
        if (d->macro.gold > 0)
            printf("  Gold $%.0f", d->macro.gold);
        if (d->macro.sp500 > 0)
            printf("  S&P500 %.0f", d->macro.sp500);
        if (d->macro.dxy > 0)
            printf("  DXY %.1f", d->macro.dxy);
        if (d->macro.gold > 0 || d->macro.sp500 > 0 || d->macro.dxy > 0)
            printf("\n");
    }

    /* Fear & Greed */
    if (d->fear_greed.valid) {
        const char *fg_color = C_WHITE;
        if (d->fear_greed.value <= 25) fg_color = C_RED;
        else if (d->fear_greed.value <= 45) fg_color = C_YELLOW;
        else if (d->fear_greed.value >= 75) fg_color = C_GREEN;

        printf("  Fear & Greed: %s%s%d — %s" C_RESET "\n",
               C_BOLD, fg_color, d->fear_greed.value, d->fear_greed.label);
    }

    /* Sentiment */
    if (d->sentiment.valid) {
        const char *s_color = d->sentiment.overall_score > 0.1 ? C_GREEN :
                              d->sentiment.overall_score < -0.1 ? C_RED : C_YELLOW;
        printf("  Sentiment: %s%.2f" C_RESET " (bull %.0f%% bear %.0f%% tweets %d)\n",
               s_color, d->sentiment.overall_score,
               d->sentiment.bullish_pct, d->sentiment.bearish_pct,
               d->sentiment.total_tweets);
    }

    /* ── Strategies ───────────────────────────────────────────────────── */
    if (d->n_strategies > 0) {
        print_header("STRATEGIES");
        for (int i = 0; i < d->n_strategies; i++) {
            printf("  %s%-20s" C_RESET " %s\n",
                   d->strategies[i].enabled ? C_GREEN : C_RED,
                   d->strategies[i].name,
                   d->strategies[i].enabled ? "ACTIVE" : "PAUSED");
        }
    }

    /* ── Advisory ─────────────────────────────────────────────────────── */
    if (d->last_advisory_ms > 0) {
        int64_t ago = (int64_t)time(NULL) - d->last_advisory_ms / 1000;
        char ago_str[32];
        format_time(ago, ago_str, sizeof(ago_str));
        printf(C_DIM "  Last advisory: %s ago" C_RESET "\n", ago_str);
    }

    print_line();
    printf(C_DIM "  Ctrl+C to stop" C_RESET "\n\n");
    fflush(stdout);
}

/* ── Background thread ──────────────────────────────────────────────────── */
static void *dashboard_thread_func(void *arg) {
    tb_dashboard_t *d = (tb_dashboard_t *)arg;

    while (d->running) {
        pthread_mutex_lock(&d->lock);
        tb_dashboard_data_t data = d->data;
        pthread_mutex_unlock(&d->lock);

        tb_dashboard_render(&data);

        /* Sleep for refresh interval */
        usleep((unsigned int)(d->refresh_ms * 1000));
    }

    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

tb_dashboard_t *tb_dashboard_create(int refresh_ms) {
    tb_dashboard_t *d = calloc(1, sizeof(tb_dashboard_t));
    if (!d) return NULL;

    d->refresh_ms = refresh_ms > 0 ? refresh_ms : 500;
    pthread_mutex_init(&d->lock, NULL);

    return d;
}

void tb_dashboard_destroy(tb_dashboard_t *d) {
    if (!d) return;
    if (d->started) tb_dashboard_stop(d);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

int tb_dashboard_start(tb_dashboard_t *d) {
    if (d->started) return 0;

    d->running = true;
    if (pthread_create(&d->thread, NULL, dashboard_thread_func, d) != 0) {
        d->running = false;
        return -1;
    }

    d->started = true;
    tb_log_info("dashboard: started (refresh=%dms)", d->refresh_ms);
    return 0;
}

void tb_dashboard_stop(tb_dashboard_t *d) {
    if (!d->started) return;
    d->running = false;
    pthread_join(d->thread, NULL);
    d->started = false;
}

void tb_dashboard_update(tb_dashboard_t *d, const tb_dashboard_data_t *data) {
    pthread_mutex_lock(&d->lock);
    d->data = *data;
    pthread_mutex_unlock(&d->lock);
}
