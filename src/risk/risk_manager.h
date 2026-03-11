#ifndef TB_RISK_MANAGER_H
#define TB_RISK_MANAGER_H

#include "core/types.h"
#include "core/config.h"
#include <stdbool.h>

typedef struct tb_risk_mgr tb_risk_mgr_t;

typedef enum {
    TB_RISK_PASS = 0,
    TB_RISK_REJECT_DAILY_LOSS,
    TB_RISK_REJECT_LEVERAGE,
    TB_RISK_REJECT_POSITION_SIZE,
    TB_RISK_REJECT_PAUSED,
    TB_RISK_REJECT_MAX_ORDERS,
    TB_RISK_REJECT_CIRCUIT_BREAKER,
} tb_risk_result_t;

tb_risk_mgr_t *tb_risk_mgr_create(const tb_config_t *cfg);
void           tb_risk_mgr_destroy(tb_risk_mgr_t *mgr);

/*
 * Pre-trade risk check. Returns TB_RISK_PASS if order is allowed.
 *
 * Checks (in order):
 *   1. Trading not paused
 *   2. Daily loss limit not breached
 *   3. Resulting leverage within max
 *   4. Position size within max USD
 */
tb_risk_result_t tb_risk_check_order(
    tb_risk_mgr_t *mgr,
    const tb_order_request_t *order,
    const tb_position_t *current_position,  /* NULL if no existing position */
    double account_value
);

/* Human-readable reason for rejection */
const char *tb_risk_result_str(tb_risk_result_t r);

/* Update daily P&L (called on each fill) */
void tb_risk_update_pnl(tb_risk_mgr_t *mgr, double realized_pnl, double fee);

/* Query daily P&L */
double tb_risk_get_daily_pnl(const tb_risk_mgr_t *mgr);

/* Pause/resume trading */
void tb_risk_pause(tb_risk_mgr_t *mgr, const char *reason);
void tb_risk_resume(tb_risk_mgr_t *mgr);
bool tb_risk_is_paused(const tb_risk_mgr_t *mgr);
void tb_risk_pause_reason(const tb_risk_mgr_t *mgr, char *buf, size_t buf_len);

/* Dynamic parameter adjustment (from AI advisory) */
void tb_risk_set_daily_limit(tb_risk_mgr_t *mgr, double limit);
void tb_risk_set_max_leverage(tb_risk_mgr_t *mgr, double max_lev);
void tb_risk_set_max_position_usd(tb_risk_mgr_t *mgr, double max_usd);

/* Check if emergency close is needed */
bool tb_risk_should_emergency_close(const tb_risk_mgr_t *mgr);

/* Reset daily counters (midnight UTC) */
void tb_risk_reset_daily(tb_risk_mgr_t *mgr);

/* Update account value for %-based limit calculations.
 * Call this periodically (e.g. every dashboard refresh). */
void tb_risk_update_account_value(tb_risk_mgr_t *mgr, double value);

/* Get current parameters (for dashboard/reports) */
double tb_risk_get_daily_limit(const tb_risk_mgr_t *mgr);
double tb_risk_get_max_leverage(const tb_risk_mgr_t *mgr);

/* ── Circuit Breaker ─────────────────────────────────────────────────────── */
/* Feed price updates; returns true if circuit breaker tripped (new entries blocked).
 * Existing position management (SL/TP/close) still allowed when tripped. */
bool tb_risk_circuit_breaker_check(tb_risk_mgr_t *mgr, const char *coin, double price);

/* Query circuit breaker state */
bool tb_risk_circuit_breaker_active(const tb_risk_mgr_t *mgr);

#endif /* TB_RISK_MANAGER_H */
