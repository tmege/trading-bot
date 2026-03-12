#ifndef TB_DASHBOARD_H
#define TB_DASHBOARD_H

#include "core/types.h"
#include "core/config.h"
#include "data/macro_fetcher.h"
#include "data/twitter_sentiment.h"
#include "data/fear_greed.h"
#include "exchange/position_tracker.h"
#include "strategy/lua_engine.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct tb_dashboard tb_dashboard_t;

/* ── Dashboard data snapshot (updated by engine) ────────────────────────── */
typedef struct {
    /* Account */
    double           account_value;
    double           daily_pnl;
    double           daily_fees;
    int              daily_trades;
    double           cumulative_pnl;
    double           cumulative_fees;

    /* Positions */
    tb_position_t    positions[TB_MAX_POSITIONS];
    int              n_positions;

    /* Open orders */
    tb_order_t       orders[128];
    int              n_orders;

    /* Market data */
    tb_macro_data_t      macro;
    tb_sentiment_data_t  sentiment;
    tb_fear_greed_t      fear_greed;

    /* Strategies */
    tb_strategy_info_t   strategies[TB_MAX_STRATEGIES];
    int                  n_strategies;

    /* System */
    int64_t          uptime_sec;
    bool             paper_mode;
} tb_dashboard_data_t;

/* Create dashboard. refresh_ms = screen refresh interval (e.g. 500). */
tb_dashboard_t *tb_dashboard_create(int refresh_ms);
void            tb_dashboard_destroy(tb_dashboard_t *d);

/* Start background rendering thread. */
int  tb_dashboard_start(tb_dashboard_t *d);
void tb_dashboard_stop(tb_dashboard_t *d);

/* Update data snapshot (thread-safe, called by engine). */
void tb_dashboard_update(tb_dashboard_t *d, const tb_dashboard_data_t *data);

/* Render once to stdout (for testing without thread). */
void tb_dashboard_render(const tb_dashboard_data_t *data);

#endif /* TB_DASHBOARD_H */
