#include "exchange/position_tracker.h"
#include "core/logging.h"
#include <string.h>
#include <time.h>
#include <math.h>

static void get_utc_date(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = gmtime_r(&now, &tm_buf);
    if (!tm) { snprintf(buf, len, "0000-00-00"); return; }
    snprintf(buf, len, "%04d-%02d-%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}

int tb_pos_tracker_init(tb_position_tracker_t *pt) {
    memset(pt, 0, sizeof(*pt));
    pthread_rwlock_init(&pt->lock, NULL);
    get_utc_date(pt->current_date, sizeof(pt->current_date));
    return 0;
}

void tb_pos_tracker_destroy(tb_position_tracker_t *pt) {
    pthread_rwlock_destroy(&pt->lock);
}

void tb_pos_tracker_on_fill(tb_position_tracker_t *pt, const tb_fill_t *fill) {
    pthread_rwlock_wrlock(&pt->lock);

    /* Check date rollover */
    char today[12];
    get_utc_date(today, sizeof(today));
    if (strcmp(today, pt->current_date) != 0) {
        tb_log_info("date rollover: %s -> %s, resetting daily counters",
                    pt->current_date, today);
        pt->daily_realized_pnl = 0.0;
        pt->daily_fees = 0.0;
        pt->daily_trade_count = 0;
        snprintf(pt->current_date, sizeof(pt->current_date), "%s", today);
    }

    /* Update daily stats */
    pt->daily_realized_pnl += tb_decimal_to_double(fill->closed_pnl);
    pt->daily_fees += fabs(tb_decimal_to_double(fill->fee));
    pt->daily_trade_count++;

    /* Update or add position */
    int found = -1;
    for (int i = 0; i < pt->n_positions; i++) {
        if (strcmp(pt->positions[i].coin, fill->coin) == 0) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        /* Update existing position size based on fill */
        tb_position_t *pos = &pt->positions[found];
        double fill_sz = tb_decimal_to_double(fill->sz);
        double fill_px = tb_decimal_to_double(fill->px);
        double prev_sz = tb_decimal_to_double(pos->size);
        double cur_sz = prev_sz;

        if (fill->side == TB_SIDE_BUY) {
            cur_sz += fill_sz;
        } else {
            cur_sz -= fill_sz;
        }

        /* Update entry price (simple average for adds, keep for reduces) */
        if (fabs(cur_sz) > 1e-12) {
            pos->size = tb_decimal_from_double(cur_sz, fill->sz.scale);
            /* Rough entry px update — REST sync will correct this */
            if ((fill->side == TB_SIDE_BUY && cur_sz > 0) ||
                (fill->side == TB_SIDE_SELL && cur_sz < 0)) {
                double old_entry = tb_decimal_to_double(pos->entry_px);
                double old_sz = fabs(prev_sz);
                if (old_sz > 0) {
                    double new_entry = (old_entry * old_sz + fill_px * fill_sz) /
                                       (old_sz + fill_sz);
                    pos->entry_px = tb_decimal_from_double(new_entry, fill->px.scale);
                }
            }
        } else {
            /* Position closed */
            if (found < pt->n_positions - 1) {
                memmove(&pt->positions[found],
                        &pt->positions[found + 1],
                        sizeof(tb_position_t) * (pt->n_positions - found - 1));
            }
            pt->n_positions--;
        }
    } else if (pt->n_positions < TB_MAX_POSITIONS) {
        /* New position */
        tb_position_t *pos = &pt->positions[pt->n_positions];
        memset(pos, 0, sizeof(*pos));
        snprintf(pos->coin, sizeof(pos->coin), "%s", fill->coin);
        pos->entry_px = fill->px;

        double sz = tb_decimal_to_double(fill->sz);
        if (fill->side == TB_SIDE_SELL) sz = -sz;
        pos->size = tb_decimal_from_double(sz, fill->sz.scale);

        pt->n_positions++;
    }

    double log_pnl = pt->daily_realized_pnl;
    double log_fees = pt->daily_fees;
    int    log_trades = pt->daily_trade_count;
    pthread_rwlock_unlock(&pt->lock);

    tb_log_info("fill: %s %s %.4f @ %.2f | daily_pnl=%.2f fees=%.2f trades=%d",
                fill->side == TB_SIDE_BUY ? "BUY" : "SELL",
                fill->coin,
                tb_decimal_to_double(fill->sz),
                tb_decimal_to_double(fill->px),
                log_pnl, log_fees, log_trades);
}

void tb_pos_tracker_sync(tb_position_tracker_t *pt, const tb_account_t *account) {
    pthread_rwlock_wrlock(&pt->lock);

    pt->account_value = tb_decimal_to_double(account->account_value);
    pt->n_positions = account->n_positions;
    memcpy(pt->positions, account->positions,
           sizeof(tb_position_t) * account->n_positions);

    pthread_rwlock_unlock(&pt->lock);

    tb_log_debug("position sync: %d positions, account_value=%.2f",
                 pt->n_positions, pt->account_value);
}

int tb_pos_tracker_get(const tb_position_tracker_t *pt, const char *coin,
                       tb_position_t *out) {
    pthread_rwlock_rdlock((pthread_rwlock_t *)&pt->lock);

    for (int i = 0; i < pt->n_positions; i++) {
        if (strcmp(pt->positions[i].coin, coin) == 0) {
            if (out) *out = pt->positions[i];
            pthread_rwlock_unlock((pthread_rwlock_t *)&pt->lock);
            return 0;
        }
    }

    pthread_rwlock_unlock((pthread_rwlock_t *)&pt->lock);
    return -1;
}

int tb_pos_tracker_get_all(const tb_position_tracker_t *pt,
                           tb_position_t *out, int *count) {
    pthread_rwlock_rdlock((pthread_rwlock_t *)&pt->lock);
    *count = pt->n_positions;
    if (out) {
        memcpy(out, pt->positions, sizeof(tb_position_t) * pt->n_positions);
    }
    pthread_rwlock_unlock((pthread_rwlock_t *)&pt->lock);
    return 0;
}

double tb_pos_tracker_daily_pnl(const tb_position_tracker_t *pt) {
    pthread_rwlock_rdlock((pthread_rwlock_t *)&pt->lock);
    double v = pt->daily_realized_pnl;
    pthread_rwlock_unlock((pthread_rwlock_t *)&pt->lock);
    return v;
}

double tb_pos_tracker_daily_fees(const tb_position_tracker_t *pt) {
    pthread_rwlock_rdlock((pthread_rwlock_t *)&pt->lock);
    double v = pt->daily_fees;
    pthread_rwlock_unlock((pthread_rwlock_t *)&pt->lock);
    return v;
}

int tb_pos_tracker_daily_trades(const tb_position_tracker_t *pt) {
    pthread_rwlock_rdlock((pthread_rwlock_t *)&pt->lock);
    int v = pt->daily_trade_count;
    pthread_rwlock_unlock((pthread_rwlock_t *)&pt->lock);
    return v;
}

double tb_pos_tracker_account_value(const tb_position_tracker_t *pt) {
    pthread_rwlock_rdlock((pthread_rwlock_t *)&pt->lock);
    double v = pt->account_value;
    pthread_rwlock_unlock((pthread_rwlock_t *)&pt->lock);
    return v;
}

void tb_pos_tracker_reset_daily(tb_position_tracker_t *pt) {
    pthread_rwlock_wrlock(&pt->lock);
    pt->daily_realized_pnl = 0.0;
    pt->daily_fees = 0.0;
    pt->daily_trade_count = 0;
    get_utc_date(pt->current_date, sizeof(pt->current_date));
    pthread_rwlock_unlock(&pt->lock);
    tb_log_info("daily counters reset");
}
