#include "strategy/indicators.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ── SMA ────────────────────────────────────────────────────────────────── */
int tb_sma(const tb_candle_input_t *candles, int n, int period, double *out) {
    if (!candles || !out || n <= 0 || period <= 0) return -1;

    memset(out, 0, sizeof(double) * (size_t)n);

    double sum = 0;
    for (int i = 0; i < n; i++) {
        sum += candles[i].close;
        if (i >= period) {
            sum -= candles[i - period].close;
        }
        if (i >= period - 1) {
            out[i] = sum / period;
        }
    }
    return 0;
}

/* ── EMA ────────────────────────────────────────────────────────────────── */
int tb_ema(const tb_candle_input_t *candles, int n, int period, double *out) {
    if (!candles || !out || n <= 0 || period <= 0) return -1;

    memset(out, 0, sizeof(double) * (size_t)n);

    double mult = 2.0 / (period + 1);

    /* Seed with SMA for first `period` candles */
    double sum = 0;
    for (int i = 0; i < period && i < n; i++) {
        sum += candles[i].close;
    }
    if (n < period) return -1;

    out[period - 1] = sum / period;

    for (int i = period; i < n; i++) {
        out[i] = (candles[i].close - out[i - 1]) * mult + out[i - 1];
    }
    return 0;
}

/* ── EMA on raw double array (for MACD signal line) ─────────────────────── */
static void ema_on_array(const double *in, int n, int start, int period, double *out) {
    double mult = 2.0 / (period + 1);

    /* Seed */
    double sum = 0;
    int seed_end = start + period;
    if (seed_end > n) return;

    for (int i = start; i < seed_end; i++) {
        sum += in[i];
    }
    out[seed_end - 1] = sum / period;

    for (int i = seed_end; i < n; i++) {
        out[i] = (in[i] - out[i - 1]) * mult + out[i - 1];
    }
}

/* ── RSI ────────────────────────────────────────────────────────────────── */
int tb_rsi(const tb_candle_input_t *candles, int n, int period, double *out) {
    if (!candles || !out || n <= 0 || period <= 0) return -1;

    memset(out, 0, sizeof(double) * (size_t)n);
    if (n < period + 1) return -1;

    /* Initial average gain/loss */
    double avg_gain = 0, avg_loss = 0;
    for (int i = 1; i <= period; i++) {
        double change = candles[i].close - candles[i - 1].close;
        if (change > 0) avg_gain += change;
        else avg_loss += fabs(change);
    }
    avg_gain /= period;
    avg_loss /= period;

    if (avg_loss == 0)
        out[period] = 100.0;
    else
        out[period] = 100.0 - (100.0 / (1.0 + avg_gain / avg_loss));

    /* Smooth (Wilder's method) */
    for (int i = period + 1; i < n; i++) {
        double change = candles[i].close - candles[i - 1].close;
        double gain = change > 0 ? change : 0;
        double loss = change < 0 ? fabs(change) : 0;

        avg_gain = (avg_gain * (period - 1) + gain) / period;
        avg_loss = (avg_loss * (period - 1) + loss) / period;

        if (avg_loss == 0)
            out[i] = 100.0;
        else
            out[i] = 100.0 - (100.0 / (1.0 + avg_gain / avg_loss));
    }
    return 0;
}

/* ── MACD ───────────────────────────────────────────────────────────────── */
int tb_macd(const tb_candle_input_t *candles, int n,
            int fast, int slow, int signal_period,
            tb_macd_val_t *out) {
    if (!candles || !out || n <= 0) return -1;

    memset(out, 0, sizeof(tb_macd_val_t) * (size_t)n);

    double *fast_ema = calloc((size_t)n, sizeof(double));
    double *slow_ema = calloc((size_t)n, sizeof(double));
    double *macd_line = calloc((size_t)n, sizeof(double));
    double *signal_line = calloc((size_t)n, sizeof(double));
    if (!fast_ema || !slow_ema || !macd_line || !signal_line) {
        free(fast_ema); free(slow_ema); free(macd_line); free(signal_line);
        return -1;
    }

    tb_ema(candles, n, fast, fast_ema);
    tb_ema(candles, n, slow, slow_ema);

    /* MACD line = fast EMA - slow EMA */
    int start = slow - 1; /* first valid slow EMA index */
    for (int i = start; i < n; i++) {
        macd_line[i] = fast_ema[i] - slow_ema[i];
    }

    /* Signal line = EMA of MACD line */
    ema_on_array(macd_line, n, start, signal_period, signal_line);

    /* Output */
    int signal_start = start + signal_period - 1;
    for (int i = 0; i < n; i++) {
        out[i].macd_line = macd_line[i];
        out[i].signal_line = signal_line[i];
        out[i].histogram = (i >= signal_start) ?
            macd_line[i] - signal_line[i] : 0;
    }

    free(fast_ema); free(slow_ema); free(macd_line); free(signal_line);
    return 0;
}

/* ── Bollinger Bands ────────────────────────────────────────────────────── */
int tb_bollinger(const tb_candle_input_t *candles, int n,
                 int period, double std_dev_mult,
                 tb_bollinger_val_t *out) {
    if (!candles || !out || n <= 0 || period <= 0) return -1;

    memset(out, 0, sizeof(tb_bollinger_val_t) * (size_t)n);

    double *sma = calloc((size_t)n, sizeof(double));
    if (!sma) return -1;
    tb_sma(candles, n, period, sma);

    for (int i = period - 1; i < n; i++) {
        /* Standard deviation */
        double sum_sq = 0;
        for (int j = i - period + 1; j <= i; j++) {
            double diff = candles[j].close - sma[i];
            sum_sq += diff * diff;
        }
        double stddev = sqrt(sum_sq / period);

        out[i].middle = sma[i];
        out[i].upper = sma[i] + std_dev_mult * stddev;
        out[i].lower = sma[i] - std_dev_mult * stddev;
        out[i].width = sma[i] > 0 ?
            (out[i].upper - out[i].lower) / sma[i] : 0;
    }

    free(sma);
    return 0;
}

/* ── ATR ────────────────────────────────────────────────────────────────── */
int tb_atr(const tb_candle_input_t *candles, int n, int period, double *out) {
    if (!candles || !out || n <= 0 || period <= 0) return -1;

    memset(out, 0, sizeof(double) * (size_t)n);
    if (n < period + 1) return -1;

    /* True Range */
    double *tr = calloc((size_t)n, sizeof(double));
    if (!tr) return -1;

    tr[0] = candles[0].high - candles[0].low;
    for (int i = 1; i < n; i++) {
        double hl = candles[i].high - candles[i].low;
        double hc = fabs(candles[i].high - candles[i - 1].close);
        double lc = fabs(candles[i].low - candles[i - 1].close);
        tr[i] = hl;
        if (hc > tr[i]) tr[i] = hc;
        if (lc > tr[i]) tr[i] = lc;
    }

    /* First ATR = average of first `period` TRs */
    double sum = 0;
    for (int i = 0; i < period; i++) sum += tr[i];
    out[period - 1] = sum / period;

    /* Wilder's smoothing */
    for (int i = period; i < n; i++) {
        out[i] = (out[i - 1] * (period - 1) + tr[i]) / period;
    }

    free(tr);
    return 0;
}

/* ── VWAP ───────────────────────────────────────────────────────────────── */
int tb_vwap(const tb_candle_input_t *candles, int n, double *out) {
    if (!candles || !out || n <= 0) return -1;

    memset(out, 0, sizeof(double) * (size_t)n);

    double cum_tp_vol = 0;
    double cum_vol = 0;

    for (int i = 0; i < n; i++) {
        double typical_price = (candles[i].high + candles[i].low + candles[i].close) / 3.0;
        cum_tp_vol += typical_price * candles[i].volume;
        cum_vol += candles[i].volume;
        out[i] = cum_vol > 0 ? cum_tp_vol / cum_vol : typical_price;
    }
    return 0;
}

/* ── Convenience snapshot ───────────────────────────────────────────────── */
tb_indicators_snapshot_t tb_indicators_compute(const tb_candle_input_t *candles, int n) {
    tb_indicators_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));

    if (!candles || n < 2) return snap;

    int last = n - 1;
    double price = candles[last].close;

    /* Allocate temp arrays */
    double *buf = calloc((size_t)n, sizeof(double));
    if (!buf) return snap;

    /* SMA 20 */
    if (n >= 20) {
        tb_sma(candles, n, 20, buf);
        snap.sma_20 = buf[last];
    }

    /* SMA 50 */
    if (n >= 50) {
        tb_sma(candles, n, 50, buf);
        snap.sma_50 = buf[last];
    }

    /* SMA 200 */
    if (n >= 200) {
        tb_sma(candles, n, 200, buf);
        snap.sma_200 = buf[last];
        snap.above_sma_200 = price > snap.sma_200;
    }

    /* EMA 12 */
    if (n >= 12) {
        tb_ema(candles, n, 12, buf);
        snap.ema_12 = buf[last];
    }

    /* EMA 26 */
    if (n >= 26) {
        tb_ema(candles, n, 26, buf);
        snap.ema_26 = buf[last];
    }

    /* RSI 14 */
    if (n >= 15) {
        tb_rsi(candles, n, 14, buf);
        snap.rsi_14 = buf[last];
        snap.rsi_oversold = snap.rsi_14 < 30.0;
        snap.rsi_overbought = snap.rsi_14 > 70.0;
    }

    /* MACD */
    if (n >= 35) {
        tb_macd_val_t *macd_buf = calloc((size_t)n, sizeof(tb_macd_val_t));
        if (macd_buf) {
            tb_macd(candles, n, 12, 26, 9, macd_buf);
            snap.macd_line = macd_buf[last].macd_line;
            snap.macd_signal = macd_buf[last].signal_line;
            snap.macd_histogram = macd_buf[last].histogram;
            snap.macd_bullish_cross = snap.macd_line > snap.macd_signal &&
                                      snap.macd_histogram > 0;
            free(macd_buf);
        }
    }

    /* Bollinger Bands */
    if (n >= 20) {
        tb_bollinger_val_t *bb_buf = calloc((size_t)n, sizeof(tb_bollinger_val_t));
        if (bb_buf) {
            tb_bollinger(candles, n, 20, 2.0, bb_buf);
            snap.bb_upper = bb_buf[last].upper;
            snap.bb_middle = bb_buf[last].middle;
            snap.bb_lower = bb_buf[last].lower;
            snap.bb_width = bb_buf[last].width;
            snap.bb_squeeze = snap.bb_width < 0.03;
            free(bb_buf);
        }
    }

    /* ATR 14 */
    if (n >= 15) {
        tb_atr(candles, n, 14, buf);
        snap.atr_14 = buf[last];
    }

    /* VWAP */
    tb_vwap(candles, n, buf);
    snap.vwap = buf[last];

    /* Golden cross */
    if (n >= 200) {
        snap.golden_cross = snap.sma_50 > snap.sma_200;
    }

    snap.valid = true;
    free(buf);
    return snap;
}
