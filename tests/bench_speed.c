/*
 * Speed Benchmark — Trading Bot
 * Measures execution latency of critical paths.
 */

#include "core/types.h"
#include "core/logging.h"
#include "strategy/indicators.h"
#include "exchange/paper_exchange.h"
#include "risk/risk_manager.h"
#include "backtest/backtest_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ── Timing helpers ────────────────────────────────────────────────────── */
static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

#define BENCH_START() double _t0 = now_us()
#define BENCH_END(label, iters) do { \
    double _t1 = now_us(); \
    double _elapsed = _t1 - _t0; \
    double _per_op = _elapsed / (iters); \
    printf("  %-40s %8d ops  %10.1f us total  %8.3f us/op  %10.0f ops/sec\n", \
           label, (int)(iters), _elapsed, _per_op, \
           _per_op > 0 ? 1e6 / _per_op : 0); \
} while(0)

/* ── 1. Decimal arithmetic ─────────────────────────────────────────────── */
static void bench_decimal(void) {
    printf("\n== DECIMAL ARITHMETIC ==\n");

    tb_decimal_t a = tb_decimal_from_double(2000.1234, 4);
    tb_decimal_t b = tb_decimal_from_double(0.005, 6);
    int N = 1000000;

    {
        BENCH_START();
        volatile tb_decimal_t r;
        for (int i = 0; i < N; i++) r = tb_decimal_add(a, b);
        (void)r;
        BENCH_END("decimal_add", N);
    }
    {
        BENCH_START();
        volatile tb_decimal_t r;
        for (int i = 0; i < N; i++) r = tb_decimal_mul(a, b);
        (void)r;
        BENCH_END("decimal_mul", N);
    }
    {
        BENCH_START();
        volatile tb_decimal_t r;
        for (int i = 0; i < N; i++) r = tb_decimal_div(a, b);
        (void)r;
        BENCH_END("decimal_div", N);
    }
    {
        BENCH_START();
        volatile int r;
        for (int i = 0; i < N; i++) r = tb_decimal_cmp(a, b);
        (void)r;
        BENCH_END("decimal_cmp", N);
    }
    {
        char buf[64];
        BENCH_START();
        for (int i = 0; i < N; i++) tb_decimal_to_str(a, buf, sizeof(buf));
        BENCH_END("decimal_to_str", N);
    }
    {
        BENCH_START();
        volatile tb_decimal_t r;
        for (int i = 0; i < N; i++) r = tb_decimal_from_str("2000.1234");
        (void)r;
        BENCH_END("decimal_from_str", N);
    }
    {
        BENCH_START();
        volatile double r;
        for (int i = 0; i < N; i++) r = tb_decimal_to_double(a);
        (void)r;
        BENCH_END("decimal_to_double", N);
    }
}

/* ── 2. Risk check ─────────────────────────────────────────────────────── */
static void bench_risk_check(void) {
    printf("\n== RISK CHECK ==\n");

    tb_config_t cfg = {0};
    cfg.daily_loss_pct = 5.0;
    cfg.max_leverage = 3;
    cfg.max_position_pct = 200.0;
    tb_risk_mgr_t *risk = tb_risk_mgr_create(&cfg);

    tb_order_request_t order = {
        .asset = 4,
        .side = TB_SIDE_BUY,
        .price = tb_decimal_from_double(2000.0, 2),
        .size = tb_decimal_from_double(0.01, 4),
        .type = TB_ORDER_LIMIT,
        .tif = TB_TIF_GTC,
    };

    int N = 500000;
    {
        BENCH_START();
        volatile int r;
        for (int i = 0; i < N; i++)
            r = tb_risk_check_order(risk, &order, NULL, 100.0);
        (void)r;
        BENCH_END("risk_check_order (pass)", N);
    }

    /* Check with existing position */
    tb_position_t pos = {
        .size = tb_decimal_from_double(0.05, 4),
        .entry_px = tb_decimal_from_double(1950.0, 2),
    };
    {
        BENCH_START();
        volatile int r;
        for (int i = 0; i < N; i++)
            r = tb_risk_check_order(risk, &order, &pos, 100.0);
        (void)r;
        BENCH_END("risk_check_order (with pos)", N);
    }

    tb_risk_mgr_destroy(risk);
}

/* ── 3. Technical indicators ───────────────────────────────────────────── */
static void bench_indicators(void) {
    printf("\n== TECHNICAL INDICATORS ==\n");

    /* Generate 500 candles */
    int NC = 500;
    tb_candle_input_t *candles = malloc(sizeof(tb_candle_input_t) * NC);
    srand(42);
    double price = 2000.0;
    for (int i = 0; i < NC; i++) {
        double noise = ((double)(rand() % 1000) / 500.0 - 1.0) * 20.0;
        price += noise * 0.1;
        if (price < 100) price = 100;
        candles[i].open = price - noise * 0.3;
        candles[i].high = price + fabs(noise) * 0.5;
        candles[i].low = price - fabs(noise) * 0.5;
        candles[i].close = price;
        candles[i].volume = 1000 + rand() % 5000;
        candles[i].time_ms = 1700000000000LL + (long)i * 3600000;
    }

    double *out_d = malloc(sizeof(double) * NC);
    tb_macd_val_t *out_macd = malloc(sizeof(tb_macd_val_t) * NC);
    tb_bollinger_val_t *out_bb = malloc(sizeof(tb_bollinger_val_t) * NC);
    int N = 10000;

    {
        BENCH_START();
        for (int i = 0; i < N; i++) tb_sma(candles, NC, 20, out_d);
        BENCH_END("SMA(20) x500 candles", N);
    }
    {
        BENCH_START();
        for (int i = 0; i < N; i++) tb_ema(candles, NC, 12, out_d);
        BENCH_END("EMA(12) x500 candles", N);
    }
    {
        BENCH_START();
        for (int i = 0; i < N; i++) tb_rsi(candles, NC, 14, out_d);
        BENCH_END("RSI(14) x500 candles", N);
    }
    {
        BENCH_START();
        for (int i = 0; i < N; i++) tb_macd(candles, NC, 12, 26, 9, out_macd);
        BENCH_END("MACD(12,26,9) x500", N);
    }
    {
        BENCH_START();
        for (int i = 0; i < N; i++) tb_bollinger(candles, NC, 20, 2.0, out_bb);
        BENCH_END("Bollinger(20,2) x500", N);
    }
    {
        BENCH_START();
        for (int i = 0; i < N; i++) tb_atr(candles, NC, 14, out_d);
        BENCH_END("ATR(14) x500 candles", N);
    }
    {
        BENCH_START();
        for (int i = 0; i < N; i++) tb_vwap(candles, NC, out_d);
        BENCH_END("VWAP x500 candles", N);
    }
    {
        int N2 = 5000;
        BENCH_START();
        volatile tb_indicators_snapshot_t snap;
        for (int i = 0; i < N2; i++) snap = tb_indicators_compute(candles, NC);
        (void)snap;
        BENCH_END("full snapshot x500", N2);
    }

    free(candles);
    free(out_d);
    free(out_macd);
    free(out_bb);
}

/* ── 4. Paper exchange order flow ──────────────────────────────────────── */
static int fill_count = 0;
static void on_fill(const tb_fill_t *fill, void *ud) {
    (void)fill; (void)ud;
    fill_count++;
}

static void bench_paper_exchange(void) {
    printf("\n== PAPER EXCHANGE ==\n");

    tb_paper_config_t cfg = {
        .initial_balance = 10000.0,
        .maker_fee_rate = 0.0002,
        .taker_fee_rate = 0.0005,
    };
    tb_paper_exchange_t *pe = tb_paper_create(&cfg);
    tb_paper_set_fill_cb(pe, on_fill, NULL);

    int N = 100000;

    /* Benchmark: place orders */
    {
        BENCH_START();
        for (int i = 0; i < N; i++) {
            tb_order_request_t order = {
                .asset = 4,
                .side = (i % 2 == 0) ? TB_SIDE_BUY : TB_SIDE_SELL,
                .price = tb_decimal_from_double(2000.0 + (i % 100), 2),
                .size = tb_decimal_from_double(0.01, 4),
                .type = TB_ORDER_LIMIT,
                .tif = TB_TIF_GTC,
            };
            uint64_t oid;
            tb_paper_place_order(pe, &order, &oid);
            /* Cancel immediately to free slot */
            tb_paper_cancel_order(pe, 4, oid);
        }
        BENCH_END("place+cancel order", N);
    }

    /* Benchmark: place limit order + feed mid to trigger fill */
    fill_count = 0;
    int N2 = 50000;
    {
        BENCH_START();
        for (int i = 0; i < N2; i++) {
            tb_order_request_t order = {
                .asset = 4,
                .side = TB_SIDE_BUY,
                .price = tb_decimal_from_double(2001.0, 2),
                .size = tb_decimal_from_double(0.001, 4),
                .type = TB_ORDER_LIMIT,
                .tif = TB_TIF_GTC,
            };
            uint64_t oid;
            tb_paper_place_order(pe, &order, &oid);
            /* Feed mid below limit → should fill */
            tb_paper_feed_mid(pe, "ASSET4", 2000.0);
        }
        BENCH_END("place+fill cycle", N2);
    }
    printf("    Fills executed: %d\n", fill_count);

    /* Benchmark: feed_mid with no matching orders */
    {
        BENCH_START();
        for (int i = 0; i < N; i++) {
            tb_paper_feed_mid(pe, "ETH", 2000.0 + (i % 50));
        }
        BENCH_END("feed_mid (no match)", N);
    }

    tb_paper_destroy(pe);
}

/* ── 5. Backtest simulation speed ──────────────────────────────────────── */
static void bench_backtest(void) {
    printf("\n== BACKTEST ENGINE ==\n");

    /* Generate synthetic candles (30 days of 1h candles = 720) */
    int NC = 720;
    tb_candle_t *candles = malloc(sizeof(tb_candle_t) * NC);
    double price = 2000.0;
    srand(123);
    for (int i = 0; i < NC; i++) {
        double noise = ((double)(rand() % 1000) / 500.0 - 1.0) * 10.0;
        price += noise * 0.2;
        if (price < 500) price = 500;
        candles[i].open = tb_decimal_from_double(price - noise * 0.3, 2);
        candles[i].high = tb_decimal_from_double(price + fabs(noise) * 0.5, 2);
        candles[i].low = tb_decimal_from_double(price - fabs(noise) * 0.5, 2);
        candles[i].close = tb_decimal_from_double(price, 2);
        candles[i].volume = tb_decimal_from_double(1000 + rand() % 5000, 0);
        candles[i].time_open = 1700000000000LL + (int64_t)i * 3600000;
        candles[i].time_close = candles[i].time_open + 3600000 - 1;
        candles[i].n_trades = 100 + rand() % 500;
    }

    /* Backtest with grid strategy */
    tb_backtest_config_t cfg = {
        .coin = "ETH",
        .strategy_path = "strategies/bb_scalp_15m.lua",
        .initial_balance = 100.0,
        .max_leverage = 3,
        .maker_fee_rate = 0.0002,
        .taker_fee_rate = 0.0005,
        .slippage_bps = 1.0,
    };

    /* Single run benchmark */
    {
        tb_backtest_engine_t *bt = tb_backtest_create(&cfg);
        tb_backtest_load_candles(bt, candles, NC);
        tb_backtest_result_t result;

        BENCH_START();
        int rc = tb_backtest_run(bt, &result);
        BENCH_END("backtest 720 candles (grid)", 1);

        if (rc == 0) {
            printf("    Result: $%.2f → $%.2f  trades=%d  WR=%.1f%%  Sharpe=%.2f\n",
                   result.start_balance, result.end_balance,
                   result.total_trades, result.win_rate, result.sharpe_ratio);
        } else {
            printf("    Backtest failed (rc=%d)\n", rc);
        }
        tb_backtest_destroy(bt);
    }

    /* Multiple runs benchmark */
    int N = 10;
    {
        BENCH_START();
        for (int i = 0; i < N; i++) {
            tb_backtest_engine_t *bt = tb_backtest_create(&cfg);
            tb_backtest_load_candles(bt, candles, NC);
            tb_backtest_result_t result;
            tb_backtest_run(bt, &result);
            tb_backtest_destroy(bt);
        }
        BENCH_END("backtest x10 (create+run+destroy)", N);
    }

    free(candles);
}

/* ── 6. Order type conversion throughput ───────────────────────────────── */
static void bench_conversions(void) {
    printf("\n== TYPE CONVERSIONS ==\n");

    int N = 1000000;

    /* from_double → to_double roundtrip */
    {
        BENCH_START();
        volatile double r;
        for (int i = 0; i < N; i++) {
            tb_decimal_t d = tb_decimal_from_double(2000.0 + i * 0.01, 4);
            r = tb_decimal_to_double(d);
        }
        (void)r;
        BENCH_END("from_double+to_double", N);
    }

    /* from_str → to_str roundtrip */
    {
        char buf[64];
        BENCH_START();
        for (int i = 0; i < N; i++) {
            tb_decimal_t d = tb_decimal_from_str("2000.1234");
            tb_decimal_to_str(d, buf, sizeof(buf));
        }
        BENCH_END("from_str+to_str", N);
    }
}

/* ── Main ──────────────────────────────────────────────────────────────── */
int main(void) {
    tb_log_init("./logs", 3); /* level 3 = errors only during bench */

    printf("\n");
    printf("========================================\n");
    printf("  TRADING BOT — SPEED BENCHMARK\n");
    printf("========================================\n");

    bench_decimal();
    bench_risk_check();
    bench_indicators();
    bench_paper_exchange();
    bench_backtest();
    bench_conversions();

    printf("\n========================================\n");
    printf("  BENCHMARK COMPLETE\n");
    printf("========================================\n\n");

    tb_log_shutdown();
    return 0;
}
