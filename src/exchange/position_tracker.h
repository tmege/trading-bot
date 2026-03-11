#ifndef TB_POSITION_TRACKER_H
#define TB_POSITION_TRACKER_H

#include "core/types.h"
#include <pthread.h>
#include <stdbool.h>

typedef struct {
    tb_position_t    positions[TB_MAX_POSITIONS];
    int              n_positions;
    pthread_rwlock_t lock;

    /* Daily P&L tracking (resets at midnight UTC) */
    double           daily_realized_pnl;
    double           daily_fees;
    int              daily_trade_count;
    char             current_date[12]; /* YYYY-MM-DD */

    /* Cumulative P&L (never resets, loaded from DB on startup) */
    double           cumulative_pnl;
    double           cumulative_fees;

    /* Total account value (updated from REST sync) */
    double           account_value;
} tb_position_tracker_t;

int  tb_pos_tracker_init(tb_position_tracker_t *pt);
void tb_pos_tracker_destroy(tb_position_tracker_t *pt);

/* Update from a fill event */
void tb_pos_tracker_on_fill(tb_position_tracker_t *pt, const tb_fill_t *fill);

/* Full sync from REST clearinghouseState */
void tb_pos_tracker_sync(tb_position_tracker_t *pt, const tb_account_t *account);

/* Query position for a coin (thread-safe). Returns 0 if found, -1 if not. */
int  tb_pos_tracker_get(const tb_position_tracker_t *pt, const char *coin,
                        tb_position_t *out);

/* Get all positions */
int  tb_pos_tracker_get_all(const tb_position_tracker_t *pt,
                            tb_position_t *out, int *count);

/* Daily P&L */
double tb_pos_tracker_daily_pnl(const tb_position_tracker_t *pt);
double tb_pos_tracker_daily_fees(const tb_position_tracker_t *pt);
int    tb_pos_tracker_daily_trades(const tb_position_tracker_t *pt);
double tb_pos_tracker_account_value(const tb_position_tracker_t *pt);

/* Cumulative P&L (all-time, loaded from DB) */
double tb_pos_tracker_cumulative_pnl(const tb_position_tracker_t *pt);
double tb_pos_tracker_cumulative_fees(const tb_position_tracker_t *pt);

/* Load cumulative totals from DB on startup */
struct sqlite3;
void tb_pos_tracker_load_cumulative(tb_position_tracker_t *pt, struct sqlite3 *db);

/* Reset daily counters (call at midnight UTC) */
void tb_pos_tracker_reset_daily(tb_position_tracker_t *pt);

#endif /* TB_POSITION_TRACKER_H */
