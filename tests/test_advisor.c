/*
 * Tests for AI Advisory (Phase 8)
 */

#include "data/ai_advisor.h"
#include "core/logging.h"
#include "core/config.h"
#include "core/db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* ── Test: advisor creation ─────────────────────────────────────────────── */
static void test_advisor_create(void) {
    printf("\n== AI Advisor Create ==\n");

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.claude_model, sizeof(cfg.claude_model), "claude-haiku-4-5-20251001");
    cfg.advisory_hour_morning = 8;
    cfg.advisory_hour_evening = 20;

    sqlite3 *db = NULL;
    tb_db_open(&db, ":memory:");

    tb_ai_advisor_t *adv = tb_ai_advisor_create(&cfg, db);
    ASSERT(adv != NULL, "advisor created (no API key)");
    ASSERT(tb_ai_advisor_last_call_time(adv) == 0, "no calls yet");

    /* Without API key, call_now should fail gracefully */
    int rc = tb_ai_advisor_call_now(adv);
    ASSERT(rc != 0, "call_now fails without API key");

    tb_ai_advisor_destroy(adv);
    tb_db_close(db);
}

/* ── Test: context update ───────────────────────────────────────────────── */
static void test_context_update(void) {
    printf("\n== AI Advisor Context ==\n");

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.claude_model, sizeof(cfg.claude_model), "claude-haiku-4-5-20251001");

    tb_ai_advisor_t *adv = tb_ai_advisor_create(&cfg, NULL);

    tb_advisory_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.account_value = 100.0;
    ctx.daily_pnl = -2.5;
    ctx.daily_trades = 5;
    ctx.fear_greed.value = 25;
    snprintf(ctx.fear_greed.label, sizeof(ctx.fear_greed.label), "Extreme Fear");
    ctx.macro.btc_price = 69000.0;
    ctx.macro.btc_dominance = 56.7;
    ctx.n_strategies = 2;
    snprintf(ctx.strategies[0].name, sizeof(ctx.strategies[0].name), "grid_eth");
    ctx.strategies[0].enabled = true;
    snprintf(ctx.strategies[1].name, sizeof(ctx.strategies[1].name), "signal_doge");
    ctx.strategies[1].enabled = true;

    tb_ai_advisor_update_context(adv, &ctx);
    ASSERT(1, "context updated without crash");

    tb_ai_advisor_destroy(adv);
}

/* ── Test: advisor with API key (live, only if env set) ─────────────────── */
static void test_advisor_live(void) {
    printf("\n== AI Advisor Live ==\n");

    const char *api_key = getenv("TB_CLAUDE_API_KEY");
    if (!api_key || api_key[0] == '\0') {
        printf("  SKIP: TB_CLAUDE_API_KEY not set\n");
        tests_passed++;
        return;
    }

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.claude_api_key, sizeof(cfg.claude_api_key), "%s", api_key);
    snprintf(cfg.claude_model, sizeof(cfg.claude_model), "claude-haiku-4-5-20251001");

    sqlite3 *db = NULL;
    tb_db_open(&db, ":memory:");

    tb_ai_advisor_t *adv = tb_ai_advisor_create(&cfg, db);

    /* Set up minimal context */
    tb_advisory_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.account_value = 100.0;
    ctx.daily_pnl = 0;
    ctx.fear_greed.value = 25;
    snprintf(ctx.fear_greed.label, sizeof(ctx.fear_greed.label), "Extreme Fear");
    ctx.macro.btc_price = 69000.0;
    ctx.macro.btc_dominance = 56.7;
    ctx.macro.eth_btc = 0.0295;
    ctx.n_strategies = 2;
    snprintf(ctx.strategies[0].name, sizeof(ctx.strategies[0].name), "grid_eth");
    ctx.strategies[0].enabled = true;
    snprintf(ctx.strategies[1].name, sizeof(ctx.strategies[1].name), "signal_doge");
    ctx.strategies[1].enabled = true;
    tb_ai_advisor_update_context(adv, &ctx);

    int rc = tb_ai_advisor_call_now(adv);
    ASSERT(rc == 0, "live Claude API call succeeded");
    ASSERT(tb_ai_advisor_last_call_time(adv) > 0, "last call time updated");

    tb_ai_advisor_destroy(adv);
    tb_db_close(db);
}

/* ── Test: thread start/stop ────────────────────────────────────────────── */
static void test_advisor_thread(void) {
    printf("\n== AI Advisor Thread ==\n");

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.claude_model, sizeof(cfg.claude_model), "claude-haiku-4-5-20251001");
    cfg.advisory_hour_morning = 8;
    cfg.advisory_hour_evening = 20;

    tb_ai_advisor_t *adv = tb_ai_advisor_create(&cfg, NULL);

    int rc = tb_ai_advisor_start(adv);
    ASSERT(rc == 0, "advisory thread started");

    /* Let it run briefly */
    usleep(100000); /* 100ms */

    tb_ai_advisor_stop(adv);
    ASSERT(1, "advisory thread stopped cleanly");

    tb_ai_advisor_destroy(adv);
}

int main(void) {
    tb_log_init("./logs", 1);

    test_advisor_create();
    test_context_update();
    test_advisor_live();
    test_advisor_thread();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    tb_log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
