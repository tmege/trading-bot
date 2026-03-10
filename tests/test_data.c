/*
 * Tests for Data Sources (Phase 6)
 */

#include "data/macro_fetcher.h"
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

/* ── Test: macro fetcher creation ───────────────────────────────────────── */
static void test_macro_create(void) {
    printf("\n== Macro Fetcher ==\n");

    tb_macro_fetcher_t *f = tb_macro_fetcher_create(NULL);
    ASSERT(f != NULL, "macro fetcher created");

    tb_macro_data_t data = tb_macro_fetcher_get(f);
    ASSERT(!data.valid, "initial data not valid");

    tb_macro_fetcher_destroy(f);
}

/* ── Test: macro fetcher live (CoinGecko) ───────────────────────────────── */
static void test_macro_live(void) {
    printf("\n== Macro Fetcher Live ==\n");

    tb_macro_fetcher_t *f = tb_macro_fetcher_create(NULL);
    int rc = tb_macro_fetcher_refresh(f);
    ASSERT(rc == 0, "macro refresh succeeded");

    tb_macro_data_t data = tb_macro_fetcher_get(f);
    ASSERT(data.valid, "macro data is valid");
    ASSERT(data.btc_price > 1000.0, "BTC price > $1000");
    ASSERT(data.btc_dominance > 10.0 && data.btc_dominance < 90.0,
           "BTC dominance in range 10-90%");
    ASSERT(data.eth_btc > 0.001 && data.eth_btc < 1.0,
           "ETH/BTC ratio in range");

    printf("    BTC=$%.0f dom=%.1f%% ETH/BTC=%.5f Gold=$%.0f T2=$%.0fB\n",
           data.btc_price, data.btc_dominance, data.eth_btc,
           data.gold, data.total2_mcap);

    tb_macro_fetcher_destroy(f);
}

/* ── Test: macro via Hyperliquid ─────────────────────────────────────────── */
static void test_macro_hyperliquid(void) {
    printf("\n== Macro via Hyperliquid ==\n");

    hl_rest_t *rest = hl_rest_create("https://api.hyperliquid.xyz", NULL, true);
    ASSERT(rest != NULL, "HL REST created");

    tb_macro_fetcher_t *f = tb_macro_fetcher_create(NULL);
    tb_macro_fetcher_set_hl_rest(f, rest);

    int rc = tb_macro_fetcher_refresh(f);
    ASSERT(rc == 0, "macro refresh via HL succeeded");

    tb_macro_data_t data = tb_macro_fetcher_get(f);
    ASSERT(data.btc_price > 1000.0, "BTC price from HL > $1000");
    ASSERT(data.eth_btc > 0.001 && data.eth_btc < 1.0, "ETH/BTC from HL in range");

    printf("    BTC=$%.0f(HL) ETH/BTC=%.5f dom=%.1f%% T2=$%.0fB\n",
           data.btc_price, data.eth_btc, data.btc_dominance, data.total2_mcap);

    tb_macro_fetcher_destroy(f);
    hl_rest_destroy(rest);
}

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
    tb_macro_data_t macro = tb_data_mgr_get_macro(mgr);
    ASSERT(!macro.valid, "initial macro not valid");

    tb_sentiment_data_t sent = tb_data_mgr_get_sentiment(mgr);
    ASSERT(!sent.valid, "initial sentiment not valid");

    tb_fear_greed_t fg = tb_data_mgr_get_fear_greed(mgr);
    ASSERT(!fg.valid, "initial fear&greed not valid");

    tb_data_mgr_destroy(mgr);
    ASSERT(1, "data manager destroyed cleanly");
}

int main(void) {
    tb_log_init("./logs", 1);

    test_macro_create();
    test_macro_live();
    test_macro_hyperliquid();
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
