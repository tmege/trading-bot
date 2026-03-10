/*
 * Tests for Technical Indicators
 */

#include "strategy/indicators.h"
#include "core/logging.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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

#define NEAR(a, b, eps) (fabs((a) - (b)) < (eps))

/* ── Generate synthetic candle data ─────────────────────────────────────── */
static void generate_candles(tb_candle_input_t *out, int n,
                              double start_price, double trend, double volatility) {
    double price = start_price;
    for (int i = 0; i < n; i++) {
        double noise = ((double)(rand() % 1000) / 1000.0 - 0.5) * volatility;
        price += trend + noise;
        if (price < 1.0) price = 1.0;

        out[i].open = price - noise * 0.3;
        out[i].high = price + fabs(noise) * 0.5;
        out[i].low = price - fabs(noise) * 0.5;
        out[i].close = price;
        out[i].volume = 1000.0 + (rand() % 5000);
        out[i].time_ms = (long)(1700000000000LL + (long)i * 3600000);
    }
}

/* ── Test SMA ───────────────────────────────────────────────────────────── */
static void test_sma(void) {
    printf("\n== SMA ==\n");

    /* Simple test: constant price → SMA = price */
    tb_candle_input_t candles[10];
    for (int i = 0; i < 10; i++) {
        candles[i].close = 100.0;
        candles[i].open = candles[i].high = candles[i].low = 100.0;
        candles[i].volume = 1000;
    }

    double out[10];
    int rc = tb_sma(candles, 10, 5, out);
    ASSERT(rc == 0, "SMA computed");
    ASSERT(NEAR(out[4], 100.0, 0.01), "SMA(5) of constant = 100");
    ASSERT(NEAR(out[9], 100.0, 0.01), "SMA(5) last = 100");
    ASSERT(out[3] == 0, "SMA(5) not enough data at index 3");

    /* Known sequence: 1,2,3,4,5 → SMA(3) at idx 2 = (1+2+3)/3 = 2 */
    for (int i = 0; i < 5; i++) candles[i].close = (double)(i + 1);
    tb_sma(candles, 5, 3, out);
    ASSERT(NEAR(out[2], 2.0, 0.01), "SMA(3) of 1,2,3 = 2.0");
    ASSERT(NEAR(out[3], 3.0, 0.01), "SMA(3) of 2,3,4 = 3.0");
    ASSERT(NEAR(out[4], 4.0, 0.01), "SMA(3) of 3,4,5 = 4.0");
}

/* ── Test EMA ───────────────────────────────────────────────────────────── */
static void test_ema(void) {
    printf("\n== EMA ==\n");

    tb_candle_input_t candles[20];
    for (int i = 0; i < 20; i++) {
        candles[i].close = 100.0;
        candles[i].open = candles[i].high = candles[i].low = 100.0;
        candles[i].volume = 1000;
    }

    double out[20];
    int rc = tb_ema(candles, 20, 10, out);
    ASSERT(rc == 0, "EMA computed");
    ASSERT(NEAR(out[9], 100.0, 0.01), "EMA(10) of constant = 100");
    ASSERT(NEAR(out[19], 100.0, 0.01), "EMA(10) last = 100");

    /* EMA reacts to price change */
    candles[15].close = 110.0;
    tb_ema(candles, 20, 10, out);
    ASSERT(out[15] > 100.0, "EMA reacts to price spike");
    ASSERT(out[19] > 100.0 && out[19] < 110.0, "EMA decays after spike");
}

/* ── Test RSI ───────────────────────────────────────────────────────────── */
static void test_rsi(void) {
    printf("\n== RSI ==\n");

    /* Steadily rising → RSI should be high */
    tb_candle_input_t candles[30];
    for (int i = 0; i < 30; i++) {
        candles[i].close = 100.0 + i * 1.0;
        candles[i].open = candles[i].close - 0.5;
        candles[i].high = candles[i].close + 0.5;
        candles[i].low = candles[i].close - 0.5;
        candles[i].volume = 1000;
    }

    double out[30];
    int rc = tb_rsi(candles, 30, 14, out);
    ASSERT(rc == 0, "RSI computed");
    ASSERT(out[29] > 80.0, "RSI > 80 on steady uptrend");

    /* Steadily falling → RSI should be low */
    for (int i = 0; i < 30; i++) {
        candles[i].close = 200.0 - i * 1.0;
    }
    tb_rsi(candles, 30, 14, out);
    ASSERT(out[29] < 20.0, "RSI < 20 on steady downtrend");

    /* Flat → RSI ~50 */
    for (int i = 0; i < 30; i++) {
        candles[i].close = 100.0 + (i % 2 == 0 ? 0.5 : -0.5);
    }
    tb_rsi(candles, 30, 14, out);
    ASSERT(out[29] > 40.0 && out[29] < 60.0, "RSI ~50 on flat market");
}

/* ── Test Bollinger Bands ───────────────────────────────────────────────── */
static void test_bollinger(void) {
    printf("\n== Bollinger Bands ==\n");

    tb_candle_input_t candles[30];
    for (int i = 0; i < 30; i++) {
        candles[i].close = 100.0;
        candles[i].open = candles[i].high = candles[i].low = 100.0;
        candles[i].volume = 1000;
    }

    tb_bollinger_val_t out[30];
    int rc = tb_bollinger(candles, 30, 20, 2.0, out);
    ASSERT(rc == 0, "Bollinger computed");
    ASSERT(NEAR(out[29].middle, 100.0, 0.01), "BB middle = 100 (constant)");
    ASSERT(NEAR(out[29].upper, 100.0, 0.01), "BB upper = 100 (no volatility)");
    ASSERT(NEAR(out[29].lower, 100.0, 0.01), "BB lower = 100 (no volatility)");

    /* With volatility, bands should widen */
    for (int i = 0; i < 30; i++) {
        candles[i].close = 100.0 + (i % 2 == 0 ? 5.0 : -5.0);
    }
    tb_bollinger(candles, 30, 20, 2.0, out);
    ASSERT(out[29].upper > out[29].middle, "BB upper > middle with volatility");
    ASSERT(out[29].lower < out[29].middle, "BB lower < middle with volatility");
    ASSERT(out[29].width > 0, "BB width > 0");
}

/* ── Test ATR ───────────────────────────────────────────────────────────── */
static void test_atr(void) {
    printf("\n== ATR ==\n");

    tb_candle_input_t candles[20];
    for (int i = 0; i < 20; i++) {
        candles[i].open = 100.0;
        candles[i].high = 102.0;
        candles[i].low = 98.0;
        candles[i].close = 100.0;
        candles[i].volume = 1000;
    }

    double out[20];
    int rc = tb_atr(candles, 20, 14, out);
    ASSERT(rc == 0, "ATR computed");
    ASSERT(out[19] > 3.5 && out[19] < 4.5, "ATR ~4 for $4 range candles");

    /* Higher volatility → higher ATR */
    for (int i = 10; i < 20; i++) {
        candles[i].high = 110.0;
        candles[i].low = 90.0;
    }
    tb_atr(candles, 20, 14, out);
    ASSERT(out[19] > 4.0, "ATR increases with higher volatility");
}

/* ── Test VWAP ──────────────────────────────────────────────────────────── */
static void test_vwap(void) {
    printf("\n== VWAP ==\n");

    tb_candle_input_t candles[10];
    for (int i = 0; i < 10; i++) {
        candles[i].high = 102.0;
        candles[i].low = 98.0;
        candles[i].close = 100.0;
        candles[i].volume = 1000.0;
    }

    double out[10];
    int rc = tb_vwap(candles, 10, out);
    ASSERT(rc == 0, "VWAP computed");
    ASSERT(NEAR(out[9], 100.0, 0.5), "VWAP ~100 for uniform candles");
}

/* ── Test MACD ──────────────────────────────────────────────────────────── */
static void test_macd(void) {
    printf("\n== MACD ==\n");

    tb_candle_input_t candles[50];
    /* Uptrend then downtrend */
    for (int i = 0; i < 25; i++) {
        candles[i].close = 100.0 + i * 2.0;
        candles[i].open = candles[i].close - 1;
        candles[i].high = candles[i].close + 1;
        candles[i].low = candles[i].close - 1;
        candles[i].volume = 1000;
    }
    for (int i = 25; i < 50; i++) {
        candles[i].close = 148.0 - (i - 25) * 2.0;
        candles[i].open = candles[i].close + 1;
        candles[i].high = candles[i].close + 1;
        candles[i].low = candles[i].close - 1;
        candles[i].volume = 1000;
    }

    tb_macd_val_t out[50];
    int rc = tb_macd(candles, 50, 12, 26, 9, out);
    ASSERT(rc == 0, "MACD computed");
    /* During uptrend, MACD line should be positive */
    ASSERT(out[30].macd_line > 0, "MACD positive during uptrend phase");
    /* At end of downtrend, MACD should be negative */
    ASSERT(out[49].macd_line < 0, "MACD negative after downtrend");
}

/* ── Test full snapshot ─────────────────────────────────────────────────── */
static void test_snapshot(void) {
    printf("\n== Full Snapshot ==\n");

    srand(42);
    tb_candle_input_t candles[250];
    generate_candles(candles, 250, 2000.0, 0.5, 20.0);

    tb_indicators_snapshot_t snap = tb_indicators_compute(candles, 250);
    ASSERT(snap.valid, "snapshot is valid");
    ASSERT(snap.sma_20 > 0, "SMA20 computed");
    ASSERT(snap.sma_50 > 0, "SMA50 computed");
    ASSERT(snap.sma_200 > 0, "SMA200 computed");
    ASSERT(snap.ema_12 > 0, "EMA12 computed");
    ASSERT(snap.rsi_14 > 0 && snap.rsi_14 < 100, "RSI in 0-100");
    ASSERT(snap.bb_upper > snap.bb_lower, "BB upper > lower");
    ASSERT(snap.atr_14 > 0, "ATR > 0");
    ASSERT(snap.vwap > 0, "VWAP > 0");

    printf("    SMA20=%.2f SMA50=%.2f SMA200=%.2f\n",
           snap.sma_20, snap.sma_50, snap.sma_200);
    printf("    EMA12=%.2f EMA26=%.2f\n", snap.ema_12, snap.ema_26);
    printf("    RSI=%.1f MACD=%.2f Signal=%.2f Hist=%.2f\n",
           snap.rsi_14, snap.macd_line, snap.macd_signal, snap.macd_histogram);
    printf("    BB: %.2f / %.2f / %.2f (width=%.4f)\n",
           snap.bb_upper, snap.bb_middle, snap.bb_lower, snap.bb_width);
    printf("    ATR=%.2f VWAP=%.2f\n", snap.atr_14, snap.vwap);
    printf("    Signals: SMA200=%s GoldenX=%s RSI_OS=%s RSI_OB=%s BB_SQ=%s MACD_Bull=%s\n",
           snap.above_sma_200 ? "above" : "below",
           snap.golden_cross ? "yes" : "no",
           snap.rsi_oversold ? "yes" : "no",
           snap.rsi_overbought ? "yes" : "no",
           snap.bb_squeeze ? "yes" : "no",
           snap.macd_bullish_cross ? "yes" : "no");
}

/* ── Test with too few candles ──────────────────────────────────────────── */
static void test_insufficient_data(void) {
    printf("\n== Insufficient Data ==\n");

    tb_candle_input_t candles[5];
    for (int i = 0; i < 5; i++) {
        candles[i].close = 100.0;
        candles[i].open = candles[i].high = candles[i].low = 100.0;
        candles[i].volume = 1000;
    }

    tb_indicators_snapshot_t snap = tb_indicators_compute(candles, 5);
    ASSERT(snap.valid, "snapshot valid even with few candles");
    ASSERT(snap.sma_200 == 0, "SMA200 = 0 with 5 candles");
    ASSERT(snap.vwap > 0, "VWAP works with 5 candles");
}

int main(void) {
    tb_log_init("./logs", 1);

    test_sma();
    test_ema();
    test_rsi();
    test_bollinger();
    test_atr();
    test_vwap();
    test_macd();
    test_snapshot();
    test_insufficient_data();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    tb_log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
