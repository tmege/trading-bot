#include "risk/risk_manager.h"
#include "core/logging.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

/* ── Circuit breaker config ─────────────────────────────────────────────── */
#define CB_MAX_COINS        16
#define CB_HISTORY_SIZE     60      /* track last 60 price snapshots */
#define CB_SAMPLE_INTERVAL  1       /* 1 second between samples */
#define CB_MOVE_THRESHOLD   15.0    /* trip if >15% move in window */
#define CB_WINDOW_SEC       60      /* detection window in seconds */
#define CB_COOLDOWN_SEC     300     /* block new entries for 5 min after trip */

typedef struct {
    char    coin[16];
    double  prices[CB_HISTORY_SIZE];
    time_t  timestamps[CB_HISTORY_SIZE];
    int     head;                   /* circular buffer head */
    int     count;
} cb_coin_history_t;

struct tb_risk_mgr {
    /* Percentage-based limits (scale with account) */
    double          daily_loss_pct;       /* e.g. 15.0 → lose max 15% of account/day */
    double          emergency_close_pct;  /* e.g. 12.0 → emergency at 12% loss */
    double          max_leverage;         /* e.g. 5.0 */
    double          max_position_pct;     /* e.g. 200.0 → max 200% of account */

    /* Dynamic account reference (updated periodically by engine) */
    double          account_value;        /* current account equity */

    /* Daily state */
    double          daily_realized_pnl;
    double          daily_fees;

    /* Pause state */
    bool            paused;
    bool            emergency_triggered;
    char            pause_reason[256];

    /* Circuit breaker */
    cb_coin_history_t cb_coins[CB_MAX_COINS];
    int               cb_n_coins;
    bool              cb_tripped;
    time_t            cb_trip_time;
    char              cb_trip_coin[16];
    double            cb_trip_pct;

    /* Thread safety */
    pthread_mutex_t lock;
};

tb_risk_mgr_t *tb_risk_mgr_create(const tb_config_t *cfg) {
    tb_risk_mgr_t *mgr = calloc(1, sizeof(tb_risk_mgr_t));
    if (!mgr) return NULL;

    mgr->daily_loss_pct     = cfg->daily_loss_pct;
    mgr->emergency_close_pct = cfg->emergency_close_pct;
    mgr->max_leverage       = cfg->max_leverage;
    mgr->max_position_pct   = cfg->max_position_pct;
    mgr->account_value      = 100.0;  /* default until engine updates */
    mgr->paused             = false;
    mgr->emergency_triggered = false;

    pthread_mutex_init(&mgr->lock, NULL);

    tb_log_info("risk manager: daily_loss=%.0f%%, emergency=%.0f%%, max_lev=%.1fx, "
                "max_pos=%.0f%% (all %%-based, scales with account)",
                mgr->daily_loss_pct, mgr->emergency_close_pct,
                mgr->max_leverage, mgr->max_position_pct);
    return mgr;
}

void tb_risk_mgr_destroy(tb_risk_mgr_t *mgr) {
    if (!mgr) return;
    pthread_mutex_destroy(&mgr->lock);
    free(mgr);
}

tb_risk_result_t tb_risk_check_order(
    tb_risk_mgr_t *mgr,
    const tb_order_request_t *order,
    const tb_position_t *current_position,
    double account_value
) {
    pthread_mutex_lock(&mgr->lock);

    /* 1. Paused? */
    if (mgr->paused) {
        pthread_mutex_unlock(&mgr->lock);
        return TB_RISK_REJECT_PAUSED;
    }

    /* 1b. Circuit breaker — block new entries, allow reduce_only */
    if (mgr->cb_tripped && !order->reduce_only) {
        time_t now = time(NULL);
        if (now - mgr->cb_trip_time < CB_COOLDOWN_SEC) {
            pthread_mutex_unlock(&mgr->lock);
            return TB_RISK_REJECT_CIRCUIT_BREAKER;
        }
        /* Cooldown expired, auto-reset */
        mgr->cb_tripped = false;
        tb_log_info("CIRCUIT BREAKER: auto-reset after %ds cooldown", CB_COOLDOWN_SEC);
    }

    /* 2. Daily loss limit (% of account) */
    double net_daily = mgr->daily_realized_pnl - mgr->daily_fees;
    double daily_limit_usd = -(mgr->account_value * mgr->daily_loss_pct / 100.0);
    if (net_daily <= daily_limit_usd) {
        pthread_mutex_unlock(&mgr->lock);
        tb_log_warn("risk: daily loss limit breached (%.2f <= %.2f = -%.0f%% of $%.0f)",
                    net_daily, daily_limit_usd, mgr->daily_loss_pct, mgr->account_value);
        return TB_RISK_REJECT_DAILY_LOSS;
    }

    /* 3. Leverage check */
    if (account_value > 0) {
        double order_value = tb_decimal_to_double(order->price) *
                             tb_decimal_to_double(order->size);
        double existing_value = 0;
        if (current_position) {
            existing_value = fabs(tb_decimal_to_double(current_position->size)) *
                             tb_decimal_to_double(current_position->entry_px);
        }

        /* Compute resulting notional */
        double result_notional;
        if (order->reduce_only) {
            result_notional = existing_value - order_value;
            if (result_notional < 0) result_notional = 0;
        } else {
            /* Check if same direction or opposite */
            bool same_dir = true;
            if (current_position) {
                double pos_sz = tb_decimal_to_double(current_position->size);
                same_dir = (pos_sz >= 0 && order->side == TB_SIDE_BUY) ||
                           (pos_sz < 0 && order->side == TB_SIDE_SELL);
            }
            result_notional = same_dir ?
                existing_value + order_value :
                fabs(existing_value - order_value);
        }

        double resulting_leverage = result_notional / account_value;
        if (resulting_leverage > mgr->max_leverage) {
            pthread_mutex_unlock(&mgr->lock);
            tb_log_warn("risk: leverage %.2fx > max %.2fx (notional=%.2f, acct=%.2f)",
                        resulting_leverage, mgr->max_leverage,
                        result_notional, account_value);
            return TB_RISK_REJECT_LEVERAGE;
        }
    }

    /* 4. Cumulative position size check (% of account) */
    if (!order->reduce_only) {
        double order_value = tb_decimal_to_double(order->price) *
                             tb_decimal_to_double(order->size);
        double existing_value = 0;
        if (current_position) {
            existing_value = fabs(tb_decimal_to_double(current_position->size)) *
                             tb_decimal_to_double(current_position->entry_px);
        }
        double resulting_value = existing_value + order_value;
        double max_pos_usd = mgr->account_value * mgr->max_position_pct / 100.0;
        if (resulting_value > max_pos_usd) {
            pthread_mutex_unlock(&mgr->lock);
            tb_log_warn("risk: resulting position $%.2f > max $%.2f (%.0f%% of $%.0f)",
                        resulting_value, max_pos_usd, mgr->max_position_pct, mgr->account_value);
            return TB_RISK_REJECT_POSITION_SIZE;
        }
    }

    pthread_mutex_unlock(&mgr->lock);
    return TB_RISK_PASS;
}

const char *tb_risk_result_str(tb_risk_result_t r) {
    switch (r) {
        case TB_RISK_PASS:                  return "pass";
        case TB_RISK_REJECT_DAILY_LOSS:     return "daily loss limit";
        case TB_RISK_REJECT_LEVERAGE:       return "leverage exceeded";
        case TB_RISK_REJECT_POSITION_SIZE:  return "position size exceeded";
        case TB_RISK_REJECT_PAUSED:         return "trading paused";
        case TB_RISK_REJECT_MAX_ORDERS:     return "max orders reached";
        case TB_RISK_REJECT_CIRCUIT_BREAKER: return "circuit breaker active";
        default:                            return "unknown";
    }
}

void tb_risk_update_pnl(tb_risk_mgr_t *mgr, double realized_pnl, double fee) {
    pthread_mutex_lock(&mgr->lock);
    mgr->daily_realized_pnl += realized_pnl;
    mgr->daily_fees += fabs(fee);
    double net = mgr->daily_realized_pnl - mgr->daily_fees;

    /* Compute dynamic thresholds from percentages */
    double emergency_usd = -(mgr->account_value * mgr->emergency_close_pct / 100.0);
    double daily_limit_usd = -(mgr->account_value * mgr->daily_loss_pct / 100.0);

    /* Check thresholds and apply pause atomically under lock to prevent TOCTOU */
    bool do_emergency = (net <= emergency_usd && !mgr->emergency_triggered);
    bool do_pause = (!do_emergency && net <= daily_limit_usd && !mgr->paused);
    if (do_emergency) {
        mgr->emergency_triggered = true;
        mgr->paused = true;
        snprintf(mgr->pause_reason, sizeof(mgr->pause_reason), "emergency close threshold");
    } else if (do_pause) {
        mgr->paused = true;
        snprintf(mgr->pause_reason, sizeof(mgr->pause_reason), "daily loss limit hit");
    }
    pthread_mutex_unlock(&mgr->lock);

    /* Log outside lock */
    if (do_emergency) {
        tb_log_warn("RISK: EMERGENCY CLOSE triggered! net=$%.2f <= -%.0f%% of $%.0f ($%.2f)",
                    net, mgr->emergency_close_pct, mgr->account_value, emergency_usd);
        tb_log_warn("RISK: trading PAUSED — emergency close threshold");
    } else if (do_pause) {
        tb_log_warn("RISK: daily loss limit hit! net=$%.2f <= -%.0f%% of $%.0f ($%.2f)",
                    net, mgr->daily_loss_pct, mgr->account_value, daily_limit_usd);
        tb_log_warn("RISK: trading PAUSED — daily loss limit hit");
    }
}

double tb_risk_get_daily_pnl(tb_risk_mgr_t *mgr) {
    pthread_mutex_lock(&mgr->lock);
    double pnl = mgr->daily_realized_pnl - mgr->daily_fees;
    pthread_mutex_unlock(&mgr->lock);
    return pnl;
}

void tb_risk_pause(tb_risk_mgr_t *mgr, const char *reason) {
    pthread_mutex_lock(&mgr->lock);
    mgr->paused = true;
    snprintf(mgr->pause_reason, sizeof(mgr->pause_reason), "%s", reason);
    pthread_mutex_unlock(&mgr->lock);
    tb_log_warn("RISK: trading PAUSED — %s", reason);
}

void tb_risk_resume(tb_risk_mgr_t *mgr) {
    pthread_mutex_lock(&mgr->lock);
    mgr->paused = false;
    mgr->pause_reason[0] = '\0';
    pthread_mutex_unlock(&mgr->lock);
    tb_log_info("RISK: trading RESUMED");
}

bool tb_risk_is_paused(tb_risk_mgr_t *mgr) {
    pthread_mutex_lock(&mgr->lock);
    bool p = mgr->paused;
    pthread_mutex_unlock(&mgr->lock);
    return p;
}

void tb_risk_pause_reason(tb_risk_mgr_t *mgr, char *buf, size_t buf_len) {
    pthread_mutex_lock(&mgr->lock);
    snprintf(buf, buf_len, "%s", mgr->pause_reason);
    pthread_mutex_unlock(&mgr->lock);
}

void tb_risk_set_daily_limit(tb_risk_mgr_t *mgr, double limit_pct) {
    /* Hard bounds: 1% to 50% */
    if (limit_pct < 1.0) limit_pct = 1.0;
    if (limit_pct > 50.0) limit_pct = 50.0;
    pthread_mutex_lock(&mgr->lock);
    mgr->daily_loss_pct = limit_pct;
    pthread_mutex_unlock(&mgr->lock);
    tb_log_info("risk: daily limit adjusted to %.0f%% of account", limit_pct);
}

void tb_risk_set_max_leverage(tb_risk_mgr_t *mgr, double max_lev) {
    /* Hard bounds: 1x to 10x */
    if (max_lev < 1.0) max_lev = 1.0;
    if (max_lev > 10.0) max_lev = 10.0;
    pthread_mutex_lock(&mgr->lock);
    mgr->max_leverage = max_lev;
    pthread_mutex_unlock(&mgr->lock);
    tb_log_info("risk: max leverage adjusted to %.1fx", max_lev);
}

void tb_risk_set_max_position_usd(tb_risk_mgr_t *mgr, double max_pct) {
    /* Hard bounds: 10% to 500% */
    if (max_pct < 10.0) max_pct = 10.0;
    if (max_pct > 500.0) max_pct = 500.0;
    pthread_mutex_lock(&mgr->lock);
    mgr->max_position_pct = max_pct;
    pthread_mutex_unlock(&mgr->lock);
    tb_log_info("risk: max position adjusted to %.0f%% of account", max_pct);
}

void tb_risk_update_account_value(tb_risk_mgr_t *mgr, double value) {
    if (value <= 0) return;
    pthread_mutex_lock(&mgr->lock);
    mgr->account_value = value;
    pthread_mutex_unlock(&mgr->lock);
}

bool tb_risk_should_emergency_close(tb_risk_mgr_t *mgr) {
    pthread_mutex_lock(&mgr->lock);
    double net = mgr->daily_realized_pnl - mgr->daily_fees;
    double emergency_usd = -(mgr->account_value * mgr->emergency_close_pct / 100.0);
    bool result = (net <= emergency_usd && !mgr->emergency_triggered);
    if (result) mgr->emergency_triggered = true;
    pthread_mutex_unlock(&mgr->lock);
    return result;
}

void tb_risk_reset_daily(tb_risk_mgr_t *mgr) {
    pthread_mutex_lock(&mgr->lock);
    mgr->daily_realized_pnl = 0.0;
    mgr->daily_fees = 0.0;
    mgr->emergency_triggered = false;
    if (mgr->paused &&
        (strcmp(mgr->pause_reason, "daily loss limit hit") == 0 ||
         strcmp(mgr->pause_reason, "emergency close threshold") == 0)) {
        mgr->paused = false;
        mgr->pause_reason[0] = '\0';
        tb_log_info("risk: auto-resumed after daily reset");
    }
    pthread_mutex_unlock(&mgr->lock);
    tb_log_info("risk: daily counters reset");
}

double tb_risk_get_daily_limit(tb_risk_mgr_t *mgr) {
    pthread_mutex_lock(&mgr->lock);
    double v = -(mgr->account_value * mgr->daily_loss_pct / 100.0);
    pthread_mutex_unlock(&mgr->lock);
    return v;
}

double tb_risk_get_max_leverage(tb_risk_mgr_t *mgr) {
    pthread_mutex_lock(&mgr->lock);
    double v = mgr->max_leverage;
    pthread_mutex_unlock(&mgr->lock);
    return v;
}

/* ── Circuit Breaker Implementation ──────────────────────────────────────── */

static cb_coin_history_t *cb_find_or_add(tb_risk_mgr_t *mgr, const char *coin) {
    for (int i = 0; i < mgr->cb_n_coins; i++) {
        if (strcmp(mgr->cb_coins[i].coin, coin) == 0)
            return &mgr->cb_coins[i];
    }
    if (mgr->cb_n_coins >= CB_MAX_COINS) return NULL;
    cb_coin_history_t *h = &mgr->cb_coins[mgr->cb_n_coins++];
    memset(h, 0, sizeof(*h));
    snprintf(h->coin, sizeof(h->coin), "%s", coin);
    return h;
}

bool tb_risk_circuit_breaker_check(tb_risk_mgr_t *mgr, const char *coin, double price) {
    if (price <= 0) return false;
    time_t now = time(NULL);

    pthread_mutex_lock(&mgr->lock);

    /* Auto-reset expired trip */
    if (mgr->cb_tripped && (now - mgr->cb_trip_time >= CB_COOLDOWN_SEC)) {
        mgr->cb_tripped = false;
        tb_log_info("CIRCUIT BREAKER: auto-reset after %ds cooldown", CB_COOLDOWN_SEC);
    }

    cb_coin_history_t *h = cb_find_or_add(mgr, coin);
    if (!h) {
        pthread_mutex_unlock(&mgr->lock);
        return mgr->cb_tripped;
    }

    /* Rate-limit samples to 1/sec to avoid flooding the buffer */
    if (h->count > 0) {
        int last = (h->head - 1 + CB_HISTORY_SIZE) % CB_HISTORY_SIZE;
        if (now - h->timestamps[last] < CB_SAMPLE_INTERVAL) {
            pthread_mutex_unlock(&mgr->lock);
            return mgr->cb_tripped;
        }
    }

    /* Add price sample */
    h->prices[h->head] = price;
    h->timestamps[h->head] = now;
    h->head = (h->head + 1) % CB_HISTORY_SIZE;
    if (h->count < CB_HISTORY_SIZE) h->count++;

    /* Check max move in the window against oldest available sample */
    if (h->count >= 5) {  /* need at least 5 samples */
        double min_p = price, max_p = price;
        for (int i = 0; i < h->count; i++) {
            int idx = (h->head - 1 - i + CB_HISTORY_SIZE) % CB_HISTORY_SIZE;
            /* Only look at samples within the detection window */
            if (now - h->timestamps[idx] > CB_WINDOW_SEC) break;
            if (h->prices[idx] < min_p) min_p = h->prices[idx];
            if (h->prices[idx] > max_p) max_p = h->prices[idx];
        }
        double move_pct = ((max_p - min_p) / min_p) * 100.0;
        if (move_pct >= CB_MOVE_THRESHOLD && !mgr->cb_tripped) {
            mgr->cb_tripped = true;
            mgr->cb_trip_time = now;
            mgr->cb_trip_pct = move_pct;
            snprintf(mgr->cb_trip_coin, sizeof(mgr->cb_trip_coin), "%s", coin);
            pthread_mutex_unlock(&mgr->lock);
            tb_log_warn("CIRCUIT BREAKER TRIPPED: %s moved %.1f%% in <%ds — "
                        "new entries blocked for %ds",
                        coin, move_pct, CB_HISTORY_SIZE, CB_COOLDOWN_SEC);
            return true;
        }
    }

    bool tripped = mgr->cb_tripped;
    pthread_mutex_unlock(&mgr->lock);
    return tripped;
}

bool tb_risk_circuit_breaker_active(tb_risk_mgr_t *mgr) {
    pthread_mutex_lock(&mgr->lock);
    bool active = mgr->cb_tripped;
    if (active) {
        time_t now = time(NULL);
        if (now - mgr->cb_trip_time >= CB_COOLDOWN_SEC) {
            active = false;
        }
    }
    pthread_mutex_unlock(&mgr->lock);
    return active;
}
