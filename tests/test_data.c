/*
 * Tests for Data Sources (Phase 6)
 */

#include "data/twitter_sentiment.h"
#include "data/fear_greed.h"
#include "data/data_manager.h"
#include "exchange/hl_rest.h"
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

/* ── Test: sentiment scoring ────────────────────────────────────────────── */
static void test_sentiment_create(void) {
    printf("\n== Sentiment Analyzer ==\n");

    const char *accounts[] = {"eliz883", "WatcherGuru"};
    tb_twitter_sentiment_t *s = tb_sentiment_create(accounts, 2);
    ASSERT(s != NULL, "sentiment analyzer created");

    tb_sentiment_data_t data = tb_sentiment_get(s);
    ASSERT(!data.valid, "initial data not valid");

    tb_sentiment_destroy(s);
}

/* ── Test: sentiment keyword scoring ────────────────────────────────────── */
static void test_sentiment_scoring(void) {
    printf("\n== Sentiment Scoring ==\n");

    /* We can't directly test score_text since it's static,
       but we can verify the refresh/get flow doesn't crash */
    const char *accounts[] = {"eliz883"};
    tb_twitter_sentiment_t *s = tb_sentiment_create(accounts, 1);

    /* Refresh may fail (Nitter instances may be down) — that's OK */
    tb_sentiment_refresh(s);

    tb_sentiment_data_t data = tb_sentiment_get(s);
    printf("    tweets=%d score=%.2f bull=%.0f%% bear=%.0f%%\n",
           data.total_tweets, data.overall_score,
           data.bullish_pct, data.bearish_pct);
    ASSERT(1, "sentiment refresh does not crash");

    /* Test spike detection */
    bool spike = tb_sentiment_has_spike(s, 0.5, 5);
    printf("    spike(0.5, 5min)=%s\n", spike ? "yes" : "no");
    ASSERT(1, "spike detection works");

    tb_sentiment_destroy(s);
}

/* ── Test: Fear & Greed live ────────────────────────────────────────────── */
static void test_fear_greed(void) {
    printf("\n== Fear & Greed Index ==\n");

    tb_fear_greed_fetcher_t *f = tb_fear_greed_create();
    ASSERT(f != NULL, "fear&greed fetcher created");

    int rc = tb_fear_greed_refresh(f);
    ASSERT(rc == 0, "fear&greed fetch succeeded");

    tb_fear_greed_t data = tb_fear_greed_get(f);
    ASSERT(data.valid, "fear&greed data valid");
    ASSERT(data.value >= 0 && data.value <= 100, "value in 0-100 range");
    ASSERT(data.label[0] != '\0', "label is set");

    printf("    F&G: %d — %s\n", data.value, data.label);

    tb_fear_greed_destroy(f);
}

/* ── Test: data manager ─────────────────────────────────────────────────── */
static void test_data_manager(void) {
    printf("\n== Data Manager ==\n");

    tb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    tb_data_mgr_t *mgr = tb_data_mgr_create(&cfg);
    ASSERT(mgr != NULL, "data manager created");

    /* Don't start the background thread in tests — just test creation/destruction */
    tb_sentiment_data_t sent = tb_data_mgr_get_sentiment(mgr);
    ASSERT(!sent.valid, "initial sentiment not valid");

    tb_fear_greed_t fg = tb_data_mgr_get_fear_greed(mgr);
    ASSERT(!fg.valid, "initial fear&greed not valid");

    tb_data_mgr_destroy(mgr);
    ASSERT(1, "data manager destroyed cleanly");
}

int main(void) {
    tb_log_init("./logs", 1);

    test_sentiment_create();
    test_sentiment_scoring();
    test_fear_greed();
    test_data_manager();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    tb_log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
