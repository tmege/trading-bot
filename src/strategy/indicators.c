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

/* ── ADX (Average Directional Index) ────────────────────────────────────── */
int tb_adx(const tb_candle_input_t *candles, int n, int period, tb_adx_val_t *out) {
    if (!candles || !out || n <= 0 || period <= 0) return -1;
    memset(out, 0, sizeof(tb_adx_val_t) * (size_t)n);
    if (n < period + 1) return -1;

    /* True Range, +DM, -DM */
    double *tr = calloc((size_t)n, sizeof(double));
    double *plus_dm = calloc((size_t)n, sizeof(double));
    double *minus_dm = calloc((size_t)n, sizeof(double));
    if (!tr || !plus_dm || !minus_dm) {
        free(tr); free(plus_dm); free(minus_dm);
        return -1;
    }

    for (int i = 1; i < n; i++) {
        double hl = candles[i].high - candles[i].low;
        double hc = fabs(candles[i].high - candles[i - 1].close);
        double lc = fabs(candles[i].low - candles[i - 1].close);
        tr[i] = hl;
        if (hc > tr[i]) tr[i] = hc;
        if (lc > tr[i]) tr[i] = lc;

        double up_move = candles[i].high - candles[i - 1].high;
        double dn_move = candles[i - 1].low - candles[i].low;
        plus_dm[i] = (up_move > dn_move && up_move > 0) ? up_move : 0;
        minus_dm[i] = (dn_move > up_move && dn_move > 0) ? dn_move : 0;
    }

    /* Wilder smoothing of TR, +DM, -DM */
    double smooth_tr = 0, smooth_plus = 0, smooth_minus = 0;
    for (int i = 1; i <= period; i++) {
        smooth_tr += tr[i];
        smooth_plus += plus_dm[i];
        smooth_minus += minus_dm[i];
    }

    /* DX values for ADX averaging */
    double *dx_arr = calloc((size_t)n, sizeof(double));
    if (!dx_arr) { free(tr); free(plus_dm); free(minus_dm); return -1; }

    for (int i = period; i < n; i++) {
        if (i > period) {
            smooth_tr = smooth_tr - smooth_tr / period + tr[i];
            smooth_plus = smooth_plus - smooth_plus / period + plus_dm[i];
            smooth_minus = smooth_minus - smooth_minus / period + minus_dm[i];
        }

        double pdi = smooth_tr > 0 ? (smooth_plus / smooth_tr) * 100.0 : 0;
        double mdi = smooth_tr > 0 ? (smooth_minus / smooth_tr) * 100.0 : 0;
        double di_sum = pdi + mdi;
        double dx = di_sum > 0 ? fabs(pdi - mdi) / di_sum * 100.0 : 0;

        dx_arr[i] = dx;
        out[i].plus_di = pdi;
        out[i].minus_di = mdi;
    }

    /* ADX = Wilder smoothed DX */
    double adx_sum = 0;
    int adx_start = period + period - 1;
    if (adx_start < n) {
        for (int i = period; i <= adx_start && i < n; i++) adx_sum += dx_arr[i];
        double adx_val = adx_sum / period;
        if (adx_start < n) out[adx_start].adx = adx_val;

        for (int i = adx_start + 1; i < n; i++) {
            adx_val = (adx_val * (period - 1) + dx_arr[i]) / period;
            out[i].adx = adx_val;
        }
    }

    free(tr); free(plus_dm); free(minus_dm); free(dx_arr);
    return 0;
}

/* ── Keltner Channels ──────────────────────────────────────────────────── */
int tb_keltner(const tb_candle_input_t *candles, int n,
               int ema_period, int atr_period, double mult,
               tb_keltner_val_t *out) {
    if (!candles || !out || n <= 0) return -1;
    memset(out, 0, sizeof(tb_keltner_val_t) * (size_t)n);

    double *ema_buf = calloc((size_t)n, sizeof(double));
    double *atr_buf = calloc((size_t)n, sizeof(double));
    if (!ema_buf || !atr_buf) { free(ema_buf); free(atr_buf); return -1; }

    tb_ema(candles, n, ema_period, ema_buf);
    tb_atr(candles, n, atr_period, atr_buf);

    int start = ema_period > atr_period ? ema_period : atr_period;
    for (int i = start - 1; i < n; i++) {
        out[i].middle = ema_buf[i];
        out[i].upper = ema_buf[i] + mult * atr_buf[i];
        out[i].lower = ema_buf[i] - mult * atr_buf[i];
    }

    free(ema_buf); free(atr_buf);
    return 0;
}

/* ── Donchian Channels ─────────────────────────────────────────────────── */
int tb_donchian(const tb_candle_input_t *candles, int n,
                int period, tb_donchian_val_t *out) {
    if (!candles || !out || n <= 0 || period <= 0) return -1;
    memset(out, 0, sizeof(tb_donchian_val_t) * (size_t)n);

    for (int i = period - 1; i < n; i++) {
        double hh = candles[i].high, ll = candles[i].low;
        for (int j = i - period + 1; j < i; j++) {
            if (candles[j].high > hh) hh = candles[j].high;
            if (candles[j].low < ll) ll = candles[j].low;
        }
        out[i].upper = hh;
        out[i].lower = ll;
        out[i].middle = (hh + ll) / 2.0;
    }
    return 0;
}

/* ── Stochastic RSI ────────────────────────────────────────────────────── */
int tb_stoch_rsi(const tb_candle_input_t *candles, int n,
                 int rsi_period, int stoch_period, int k_smooth, int d_smooth,
                 tb_stoch_rsi_val_t *out) {
    if (!candles || !out || n <= 0) return -1;
    memset(out, 0, sizeof(tb_stoch_rsi_val_t) * (size_t)n);

    double *rsi_buf = calloc((size_t)n, sizeof(double));
    if (!rsi_buf) return -1;
    tb_rsi(candles, n, rsi_period, rsi_buf);

    /* Raw Stochastic of RSI */
    double *raw_k = calloc((size_t)n, sizeof(double));
    if (!raw_k) { free(rsi_buf); return -1; }

    int stoch_start = rsi_period + stoch_period;
    for (int i = stoch_start; i < n; i++) {
        double min_rsi = rsi_buf[i], max_rsi = rsi_buf[i];
        for (int j = i - stoch_period + 1; j < i; j++) {
            if (rsi_buf[j] < min_rsi) min_rsi = rsi_buf[j];
            if (rsi_buf[j] > max_rsi) max_rsi = rsi_buf[j];
        }
        double range = max_rsi - min_rsi;
        raw_k[i] = range > 0 ? (rsi_buf[i] - min_rsi) / range * 100.0 : 50.0;
    }

    /* Smooth K (SMA of raw_k) */
    double *smooth_k = calloc((size_t)n, sizeof(double));
    if (!smooth_k) { free(rsi_buf); free(raw_k); return -1; }

    for (int i = stoch_start + k_smooth - 1; i < n; i++) {
        double sum = 0;
        for (int j = 0; j < k_smooth; j++) sum += raw_k[i - j];
        smooth_k[i] = sum / k_smooth;
    }

    /* D = SMA of smooth K */
    int d_start = stoch_start + k_smooth - 1 + d_smooth - 1;
    for (int i = d_start; i < n; i++) {
        double sum = 0;
        for (int j = 0; j < d_smooth; j++) sum += smooth_k[i - j];
        out[i].k = smooth_k[i];
        out[i].d = sum / d_smooth;
    }

    free(rsi_buf); free(raw_k); free(smooth_k);
    return 0;
}

/* ── CCI (Commodity Channel Index) ─────────────────────────────────────── */
int tb_cci(const tb_candle_input_t *candles, int n, int period, double *out) {
    if (!candles || !out || n <= 0 || period <= 0) return -1;
    memset(out, 0, sizeof(double) * (size_t)n);

    /* Typical price */
    double *tp = calloc((size_t)n, sizeof(double));
    if (!tp) return -1;
    for (int i = 0; i < n; i++)
        tp[i] = (candles[i].high + candles[i].low + candles[i].close) / 3.0;

    for (int i = period - 1; i < n; i++) {
        /* SMA of TP */
        double sma = 0;
        for (int j = i - period + 1; j <= i; j++) sma += tp[j];
        sma /= period;

        /* Mean deviation */
        double mean_dev = 0;
        for (int j = i - period + 1; j <= i; j++) mean_dev += fabs(tp[j] - sma);
        mean_dev /= period;

        out[i] = mean_dev > 0 ? (tp[i] - sma) / (0.015 * mean_dev) : 0;
    }

    free(tp);
    return 0;
}

/* ── Williams %R ───────────────────────────────────────────────────────── */
int tb_williams_r(const tb_candle_input_t *candles, int n, int period, double *out) {
    if (!candles || !out || n <= 0 || period <= 0) return -1;
    memset(out, 0, sizeof(double) * (size_t)n);

    for (int i = period - 1; i < n; i++) {
        double hh = candles[i].high, ll = candles[i].low;
        for (int j = i - period + 1; j < i; j++) {
            if (candles[j].high > hh) hh = candles[j].high;
            if (candles[j].low < ll) ll = candles[j].low;
        }
        double range = hh - ll;
        out[i] = range > 0 ? ((hh - candles[i].close) / range) * -100.0 : 0;
    }
    return 0;
}

/* ── OBV (On-Balance Volume) ───────────────────────────────────────────── */
int tb_obv(const tb_candle_input_t *candles, int n, double *out) {
    if (!candles || !out || n <= 0) return -1;
    memset(out, 0, sizeof(double) * (size_t)n);

    out[0] = candles[0].volume;
    for (int i = 1; i < n; i++) {
        if (candles[i].close > candles[i - 1].close)
            out[i] = out[i - 1] + candles[i].volume;
        else if (candles[i].close < candles[i - 1].close)
            out[i] = out[i - 1] - candles[i].volume;
        else
            out[i] = out[i - 1];
    }
    return 0;
}

/* ── Ichimoku Cloud ────────────────────────────────────────────────────── */
static double period_high(const tb_candle_input_t *candles, int end, int period) {
    double hh = candles[end].high;
    int start = end - period + 1;
    if (start < 0) start = 0;
    for (int i = start; i < end; i++)
        if (candles[i].high > hh) hh = candles[i].high;
    return hh;
}

static double period_low(const tb_candle_input_t *candles, int end, int period) {
    double ll = candles[end].low;
    int start = end - period + 1;
    if (start < 0) start = 0;
    for (int i = start; i < end; i++)
        if (candles[i].low < ll) ll = candles[i].low;
    return ll;
}

int tb_ichimoku(const tb_candle_input_t *candles, int n, tb_ichimoku_val_t *out) {
    if (!candles || !out || n <= 0) return -1;
    memset(out, 0, sizeof(tb_ichimoku_val_t) * (size_t)n);

    for (int i = 0; i < n; i++) {
        /* Tenkan-sen (9) */
        if (i >= 8) {
            out[i].tenkan = (period_high(candles, i, 9) + period_low(candles, i, 9)) / 2.0;
        }
        /* Kijun-sen (26) */
        if (i >= 25) {
            out[i].kijun = (period_high(candles, i, 26) + period_low(candles, i, 26)) / 2.0;
        }
        /* Senkou A = (tenkan + kijun) / 2 — value at i represents 26 periods ahead */
        if (i >= 25) {
            out[i].senkou_a = (out[i].tenkan + out[i].kijun) / 2.0;
        }
        /* Senkou B = (52H + 52L) / 2 — value at i represents 26 periods ahead */
        if (i >= 51) {
            out[i].senkou_b = (period_high(candles, i, 52) + period_low(candles, i, 52)) / 2.0;
        }
        /* Chikou = close at current bar (represents close shifted back 26) */
        out[i].chikou = candles[i].close;
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

    /* ADX 14 */
    if (n >= 28) {
        tb_adx_val_t *adx_buf = calloc((size_t)n, sizeof(tb_adx_val_t));
        if (adx_buf) {
            tb_adx(candles, n, 14, adx_buf);
            snap.adx_14 = adx_buf[last].adx;
            snap.plus_di = adx_buf[last].plus_di;
            snap.minus_di = adx_buf[last].minus_di;
            snap.adx_trending = snap.adx_14 > 25.0;
            free(adx_buf);
        }
    }

    /* Keltner Channels (EMA 20, ATR 10, mult 1.5) */
    if (n >= 20) {
        tb_keltner_val_t *kc_buf = calloc((size_t)n, sizeof(tb_keltner_val_t));
        if (kc_buf) {
            tb_keltner(candles, n, 20, 10, 1.5, kc_buf);
            snap.kc_upper = kc_buf[last].upper;
            snap.kc_middle = kc_buf[last].middle;
            snap.kc_lower = kc_buf[last].lower;
            /* KC squeeze: BB inside KC */
            snap.kc_squeeze = (snap.bb_upper < snap.kc_upper &&
                               snap.bb_lower > snap.kc_lower &&
                               snap.bb_upper > 0 && snap.kc_upper > 0);
            free(kc_buf);
        }
    }

    /* Donchian Channels (20) */
    if (n >= 20) {
        tb_donchian_val_t *dc_buf = calloc((size_t)n, sizeof(tb_donchian_val_t));
        if (dc_buf) {
            tb_donchian(candles, n, 20, dc_buf);
            snap.dc_upper = dc_buf[last].upper;
            snap.dc_lower = dc_buf[last].lower;
            snap.dc_middle = dc_buf[last].middle;
            free(dc_buf);
        }
    }

    /* Stochastic RSI (14,14,3,3) */
    if (n >= 35) {
        tb_stoch_rsi_val_t *sr_buf = calloc((size_t)n, sizeof(tb_stoch_rsi_val_t));
        if (sr_buf) {
            tb_stoch_rsi(candles, n, 14, 14, 3, 3, sr_buf);
            snap.stoch_rsi_k = sr_buf[last].k;
            snap.stoch_rsi_d = sr_buf[last].d;
            free(sr_buf);
        }
    }

    /* CCI 20 */
    if (n >= 20) {
        tb_cci(candles, n, 20, buf);
        snap.cci_20 = buf[last];
    }

    /* Williams %R 14 */
    if (n >= 14) {
        tb_williams_r(candles, n, 14, buf);
        snap.williams_r = buf[last];
    }

    /* OBV + OBV SMA 20 */
    {
        double *obv_buf = calloc((size_t)n, sizeof(double));
        if (obv_buf) {
            tb_obv(candles, n, obv_buf);
            snap.obv = obv_buf[last];
            /* OBV SMA 20 */
            if (n >= 20) {
                double obv_sum = 0;
                for (int i = last - 19; i <= last; i++) obv_sum += obv_buf[i];
                snap.obv_sma = obv_sum / 20.0;
            }
            free(obv_buf);
        }
    }

    /* Ichimoku */
    if (n >= 52) {
        tb_ichimoku_val_t *ichi_buf = calloc((size_t)n, sizeof(tb_ichimoku_val_t));
        if (ichi_buf) {
            tb_ichimoku(candles, n, ichi_buf);
            snap.ichi_tenkan = ichi_buf[last].tenkan;
            snap.ichi_kijun = ichi_buf[last].kijun;
            snap.ichi_senkou_a = ichi_buf[last].senkou_a;
            snap.ichi_senkou_b = ichi_buf[last].senkou_b;
            /* Chikou: use the value from 26 bars ago (represents current close shifted back) */
            if (last >= 26) {
                snap.ichi_chikou = ichi_buf[last - 26].chikou;
            }
            /* Bullish: price > cloud and tenkan > kijun */
            double cloud_top = snap.ichi_senkou_a > snap.ichi_senkou_b ?
                               snap.ichi_senkou_a : snap.ichi_senkou_b;
            snap.ichi_bullish = (price > cloud_top && snap.ichi_tenkan > snap.ichi_kijun);
            free(ichi_buf);
        }
    }

    snap.valid = true;
    free(buf);
    return snap;
}
