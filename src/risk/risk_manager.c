#include "risk/risk_manager.h"
#include "core/logging.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

struct tb_risk_mgr {
    /* Limits */
    double          daily_loss_limit;     /* e.g. -15.0 (negative) — hard stop */
    double          max_leverage;         /* e.g. 5.0 */
    double          per_trade_stop_pct;   /* e.g. 2.0 */
    double          max_position_usd;     /* e.g. 300.0 */
    double          emergency_close_usd;  /* e.g. -12.0 — close all before limit */

    /* Daily state */
    double          daily_realized_pnl;
    double          daily_fees;

    /* Pause state */
    bool            paused;
    bool            emergency_triggered;
    char            pause_reason[256];

    /* Thread safety */
    pthread_mutex_t lock;
};

tb_risk_mgr_t *tb_risk_mgr_create(const tb_config_t *cfg) {
    tb_risk_mgr_t *mgr = calloc(1, sizeof(tb_risk_mgr_t));
    if (!mgr) return NULL;

    mgr->daily_loss_limit   = cfg->daily_loss_limit;
    mgr->max_leverage       = cfg->max_leverage;
    mgr->per_trade_stop_pct = cfg->per_trade_stop_pct;
    mgr->max_position_usd   = cfg->max_position_usd;
    mgr->emergency_close_usd = cfg->emergency_close_usd;
    mgr->paused             = false;
    mgr->emergency_triggered = false;

    pthread_mutex_init(&mgr->lock, NULL);

    tb_log_info("risk manager: daily_limit=%.2f, emergency=%.2f, max_lev=%.1fx, stop_pct=%.1f%%, max_pos=$%.0f",
                mgr->daily_loss_limit, mgr->emergency_close_usd,
                mgr->max_leverage, mgr->per_trade_stop_pct, mgr->max_position_usd);
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

    /* 2. Daily loss limit */
    double net_daily = mgr->daily_realized_pnl - mgr->daily_fees;
    if (net_daily <= mgr->daily_loss_limit) {
        pthread_mutex_unlock(&mgr->lock);
        tb_log_warn("risk: daily loss limit breached (%.2f <= %.2f)",
                    net_daily, mgr->daily_loss_limit);
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

    /* 4. Position size check */
    {
        double order_value = tb_decimal_to_double(order->price) *
                             tb_decimal_to_double(order->size);
        if (order_value > mgr->max_position_usd && !order->reduce_only) {
            pthread_mutex_unlock(&mgr->lock);
            tb_log_warn("risk: order value $%.2f > max $%.2f",
                        order_value, mgr->max_position_usd);
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
        default:                            return "unknown";
    }
}

void tb_risk_update_pnl(tb_risk_mgr_t *mgr, double realized_pnl, double fee) {
    pthread_mutex_lock(&mgr->lock);
    mgr->daily_realized_pnl += realized_pnl;
    mgr->daily_fees += fabs(fee);
    double net = mgr->daily_realized_pnl - mgr->daily_fees;

    /* Check thresholds under lock to prevent TOCTOU race */
    bool do_emergency = (net <= mgr->emergency_close_usd && !mgr->emergency_triggered);
    bool do_pause = (!do_emergency && net <= mgr->daily_loss_limit && !mgr->paused);
    if (do_emergency) mgr->emergency_triggered = true;
    pthread_mutex_unlock(&mgr->lock);

    /* Actions outside lock to avoid deadlock with tb_risk_pause */
    if (do_emergency) {
        tb_log_warn("RISK: EMERGENCY CLOSE triggered! net=%.2f <= threshold=%.2f",
                    net, mgr->emergency_close_usd);
        tb_risk_pause(mgr, "emergency close threshold");
    } else if (do_pause) {
        tb_log_warn("RISK: daily loss limit hit! net=%.2f, limit=%.2f",
                    net, mgr->daily_loss_limit);
        tb_risk_pause(mgr, "daily loss limit hit");
    }
}

double tb_risk_get_daily_pnl(const tb_risk_mgr_t *mgr) {
    pthread_mutex_lock((pthread_mutex_t *)&mgr->lock);
    double pnl = mgr->daily_realized_pnl - mgr->daily_fees;
    pthread_mutex_unlock((pthread_mutex_t *)&mgr->lock);
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

bool tb_risk_is_paused(const tb_risk_mgr_t *mgr) {
    pthread_mutex_lock((pthread_mutex_t *)&mgr->lock);
    bool p = mgr->paused;
    pthread_mutex_unlock((pthread_mutex_t *)&mgr->lock);
    return p;
}

void tb_risk_pause_reason(const tb_risk_mgr_t *mgr, char *buf, size_t buf_len) {
    pthread_mutex_lock((pthread_mutex_t *)&mgr->lock);
    snprintf(buf, buf_len, "%s", mgr->pause_reason);
    pthread_mutex_unlock((pthread_mutex_t *)&mgr->lock);
}

void tb_risk_set_daily_limit(tb_risk_mgr_t *mgr, double limit) {
    /* Hard bounds: must be negative, floor at -100 */
    if (limit > 0.0) limit = 0.0;
    if (limit < -100.0) limit = -100.0;
    pthread_mutex_lock(&mgr->lock);
    mgr->daily_loss_limit = limit;
    pthread_mutex_unlock(&mgr->lock);
    tb_log_info("risk: daily limit adjusted to %.2f", limit);
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

void tb_risk_set_max_position_usd(tb_risk_mgr_t *mgr, double max_usd) {
    /* Hard bounds: $10 to $10000 */
    if (max_usd < 10.0) max_usd = 10.0;
    if (max_usd > 10000.0) max_usd = 10000.0;
    pthread_mutex_lock(&mgr->lock);
    mgr->max_position_usd = max_usd;
    pthread_mutex_unlock(&mgr->lock);
    tb_log_info("risk: max position adjusted to $%.0f", max_usd);
}

double tb_risk_compute_stop_price(const tb_risk_mgr_t *mgr,
                                   tb_side_t side, double entry_price) {
    double pct = mgr->per_trade_stop_pct / 100.0;
    if (side == TB_SIDE_BUY) {
        /* Long: stop below entry */
        return entry_price * (1.0 - pct);
    } else {
        /* Short: stop above entry */
        return entry_price * (1.0 + pct);
    }
}

bool tb_risk_should_emergency_close(const tb_risk_mgr_t *mgr) {
    pthread_mutex_lock((pthread_mutex_t *)&mgr->lock);
    double net = mgr->daily_realized_pnl - mgr->daily_fees;
    bool result = net <= mgr->emergency_close_usd;
    pthread_mutex_unlock((pthread_mutex_t *)&mgr->lock);
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

double tb_risk_get_daily_limit(const tb_risk_mgr_t *mgr) {
    pthread_mutex_lock((pthread_mutex_t *)&mgr->lock);
    double v = mgr->daily_loss_limit;
    pthread_mutex_unlock((pthread_mutex_t *)&mgr->lock);
    return v;
}

double tb_risk_get_max_leverage(const tb_risk_mgr_t *mgr) {
    pthread_mutex_lock((pthread_mutex_t *)&mgr->lock);
    double v = mgr->max_leverage;
    pthread_mutex_unlock((pthread_mutex_t *)&mgr->lock);
    return v;
}
