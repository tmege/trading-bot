/*
 * Tests for Risk Manager (Phase 3)
 */

#include "risk/risk_manager.h"
#include "core/logging.h"
#include "core/config.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s\n", msg); \
        tests_failed++; \
    } else { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

static void test_risk_basic(void) {
    printf("\n== Risk Manager Basic ==\n");

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.daily_loss_limit = -5.0;
    cfg.max_leverage = 3.0;
    cfg.per_trade_stop_pct = 2.0;
    cfg.max_position_usd = 200.0;

    tb_risk_mgr_t *risk = tb_risk_mgr_create(&cfg);
    ASSERT(risk != NULL, "risk manager created");

    /* Normal order should pass */
    tb_order_request_t order;
    memset(&order, 0, sizeof(order));
    order.side = TB_SIDE_BUY;
    order.price = tb_decimal_from_double(2000.0, 2);
    order.size = tb_decimal_from_double(0.05, 4);

    tb_risk_result_t rc = tb_risk_check_order(risk, &order, NULL, 100.0);
    ASSERT(rc == TB_RISK_PASS, "normal order passes risk check");

    /* Order exceeding position size limit */
    order.size = tb_decimal_from_double(0.2, 4); /* $400 > $200 limit */
    rc = tb_risk_check_order(risk, &order, NULL, 200.0); /* acct=200 → lev=2x OK, but $400>$200 max */
    ASSERT(rc == TB_RISK_REJECT_POSITION_SIZE, "oversized order rejected");

    /* Order exceeding leverage */
    order.size = tb_decimal_from_double(0.05, 4); /* $100 */
    rc = tb_risk_check_order(risk, &order, NULL, 30.0); /* $100/$30 = 3.33x > 3x */
    ASSERT(rc == TB_RISK_REJECT_LEVERAGE, "overleveraged order rejected");

    tb_risk_mgr_destroy(risk);
}

static void test_risk_daily_loss(void) {
    printf("\n== Risk Manager Daily Loss ==\n");

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.daily_loss_limit = -5.0;
    cfg.emergency_close_usd = -4.0;   /* emergency before daily limit */
    cfg.max_leverage = 3.0;
    cfg.per_trade_stop_pct = 2.0;
    cfg.max_position_usd = 200.0;

    tb_risk_mgr_t *risk = tb_risk_mgr_create(&cfg);

    /* Simulate losses */
    tb_risk_update_pnl(risk, -2.0, 0.5);
    ASSERT(tb_risk_get_daily_pnl(risk) == -2.5, "daily P&L = -2.5 after loss");
    ASSERT(!tb_risk_is_paused(risk), "not paused yet");

    tb_risk_update_pnl(risk, -3.0, 0.1);
    /* Now at -5.6 which is < -5.0 → should auto-pause */
    ASSERT(tb_risk_is_paused(risk), "auto-paused after daily limit");

    /* Orders should be rejected */
    tb_order_request_t order;
    memset(&order, 0, sizeof(order));
    order.side = TB_SIDE_BUY;
    order.price = tb_decimal_from_double(2000.0, 2);
    order.size = tb_decimal_from_double(0.01, 4);

    tb_risk_result_t rc = tb_risk_check_order(risk, &order, NULL, 100.0);
    ASSERT(rc == TB_RISK_REJECT_PAUSED, "order rejected while paused");

    /* Reset daily */
    tb_risk_reset_daily(risk);
    ASSERT(!tb_risk_is_paused(risk), "resumed after daily reset");
    ASSERT(tb_risk_get_daily_pnl(risk) == 0.0, "daily P&L reset to 0");

    /* Orders should pass again */
    rc = tb_risk_check_order(risk, &order, NULL, 100.0);
    ASSERT(rc == TB_RISK_PASS, "order passes after reset");

    tb_risk_mgr_destroy(risk);
}

static void test_risk_pause_resume(void) {
    printf("\n== Risk Manager Pause/Resume ==\n");

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.daily_loss_limit = -5.0;
    cfg.max_leverage = 3.0;
    cfg.per_trade_stop_pct = 2.0;
    cfg.max_position_usd = 200.0;

    tb_risk_mgr_t *risk = tb_risk_mgr_create(&cfg);

    /* Manual pause */
    tb_risk_pause(risk, "FOMC meeting");
    ASSERT(tb_risk_is_paused(risk), "paused after manual pause");
    {
        char reason[256];
        tb_risk_pause_reason(risk, reason, sizeof(reason));
        ASSERT(strcmp(reason, "FOMC meeting") == 0, "pause reason set");
    }

    /* Manual resume */
    tb_risk_resume(risk);
    ASSERT(!tb_risk_is_paused(risk), "resumed after manual resume");

    tb_risk_mgr_destroy(risk);
}

static void test_risk_stop_price(void) {
    printf("\n== Risk Manager Stop Price ==\n");

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.daily_loss_limit = -5.0;
    cfg.max_leverage = 3.0;
    cfg.per_trade_stop_pct = 2.0;
    cfg.max_position_usd = 200.0;

    tb_risk_mgr_t *risk = tb_risk_mgr_create(&cfg);

    /* Long stop: entry $2000, 2% stop → $1960 */
    double stop = tb_risk_compute_stop_price(risk, TB_SIDE_BUY, 2000.0);
    ASSERT(stop > 1959.9 && stop < 1960.1, "long stop at -2%");

    /* Short stop: entry $2000, 2% stop → $2040 */
    stop = tb_risk_compute_stop_price(risk, TB_SIDE_SELL, 2000.0);
    ASSERT(stop > 2039.9 && stop < 2040.1, "short stop at +2%");

    /* Dynamic adjustment */
    tb_risk_set_max_leverage(risk, 5.0);
    ASSERT(tb_risk_get_max_leverage(risk) == 5.0, "max leverage adjusted");

    tb_risk_set_daily_limit(risk, -10.0);
    ASSERT(tb_risk_get_daily_limit(risk) == -10.0, "daily limit adjusted");

    tb_risk_mgr_destroy(risk);
}

int main(void) {
    tb_log_init("./logs", 1);

    test_risk_basic();
    test_risk_daily_loss();
    test_risk_pause_resume();
    test_risk_stop_price();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    tb_log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
