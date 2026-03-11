#ifndef TB_INDICATORS_H
#define TB_INDICATORS_H

#include <stdbool.h>

/* ── Candle input (simplified for indicator computation) ────────────────── */
typedef struct {
    double open;
    double high;
    double low;
    double close;
    double volume;
    long   time_ms;
} tb_candle_input_t;

/* ── SMA / EMA ──────────────────────────────────────────────────────────── */

/* Simple Moving Average over `period` candles. Returns 0 on success.
   out[] must have space for `n_candles` values.
   First (period-1) values are set to 0 (insufficient data). */
int tb_sma(const tb_candle_input_t *candles, int n_candles,
           int period, double *out);

/* Exponential Moving Average. */
int tb_ema(const tb_candle_input_t *candles, int n_candles,
           int period, double *out);

/* ── RSI (Relative Strength Index) ──────────────────────────────────────── */

/* RSI over `period` candles (typically 14).
   out[] has n_candles values; first `period` are 0. */
int tb_rsi(const tb_candle_input_t *candles, int n_candles,
           int period, double *out);

/* ── MACD ───────────────────────────────────────────────────────────────── */
typedef struct {
    double macd_line;    /* fast EMA - slow EMA */
    double signal_line;  /* EMA of macd_line */
    double histogram;    /* macd_line - signal_line */
} tb_macd_val_t;

/* MACD with configurable periods (default 12, 26, 9).
   out[] has n_candles values. */
int tb_macd(const tb_candle_input_t *candles, int n_candles,
            int fast_period, int slow_period, int signal_period,
            tb_macd_val_t *out);

/* ── Bollinger Bands ────────────────────────────────────────────────────── */
typedef struct {
    double upper;
    double middle;   /* SMA */
    double lower;
    double width;    /* (upper - lower) / middle */
} tb_bollinger_val_t;

/* Bollinger Bands (default period=20, std_dev=2.0).
   out[] has n_candles values. */
int tb_bollinger(const tb_candle_input_t *candles, int n_candles,
                 int period, double std_dev_mult,
                 tb_bollinger_val_t *out);

/* ── ATR (Average True Range) ───────────────────────────────────────────── */

/* ATR over `period` candles (typically 14).
   out[] has n_candles values. */
int tb_atr(const tb_candle_input_t *candles, int n_candles,
           int period, double *out);

/* ── VWAP (Volume Weighted Average Price) ───────────────────────────────── */

/* VWAP: cumulative within the candle set.
   out[] has n_candles values. */
int tb_vwap(const tb_candle_input_t *candles, int n_candles, double *out);

/* ── ADX (Average Directional Index) ───────────────────────────────────── */
typedef struct {
    double adx;
    double plus_di;
    double minus_di;
} tb_adx_val_t;

int tb_adx(const tb_candle_input_t *candles, int n_candles,
           int period, tb_adx_val_t *out);

/* ── Keltner Channels ──────────────────────────────────────────────────── */
typedef struct {
    double upper;
    double middle;  /* EMA */
    double lower;
} tb_keltner_val_t;

int tb_keltner(const tb_candle_input_t *candles, int n_candles,
               int ema_period, int atr_period, double mult,
               tb_keltner_val_t *out);

/* ── Donchian Channels ─────────────────────────────────────────────────── */
typedef struct {
    double upper;   /* highest high */
    double lower;   /* lowest low */
    double middle;
} tb_donchian_val_t;

int tb_donchian(const tb_candle_input_t *candles, int n_candles,
                int period, tb_donchian_val_t *out);

/* ── Stochastic RSI ────────────────────────────────────────────────────── */
typedef struct {
    double k;
    double d;
} tb_stoch_rsi_val_t;

int tb_stoch_rsi(const tb_candle_input_t *candles, int n_candles,
                 int rsi_period, int stoch_period, int k_smooth, int d_smooth,
                 tb_stoch_rsi_val_t *out);

/* ── CCI (Commodity Channel Index) ─────────────────────────────────────── */
int tb_cci(const tb_candle_input_t *candles, int n_candles,
           int period, double *out);

/* ── Williams %R ───────────────────────────────────────────────────────── */
int tb_williams_r(const tb_candle_input_t *candles, int n_candles,
                  int period, double *out);

/* ── OBV (On-Balance Volume) ───────────────────────────────────────────── */
int tb_obv(const tb_candle_input_t *candles, int n_candles, double *out);

/* ── Ichimoku Cloud ────────────────────────────────────────────────────── */
typedef struct {
    double tenkan;     /* (9H+9L)/2 */
    double kijun;      /* (26H+26L)/2 */
    double senkou_a;   /* (tenkan+kijun)/2, shifted 26 forward */
    double senkou_b;   /* (52H+52L)/2, shifted 26 forward */
    double chikou;     /* close shifted 26 back */
} tb_ichimoku_val_t;

int tb_ichimoku(const tb_candle_input_t *candles, int n_candles,
                tb_ichimoku_val_t *out);

/* ── Convenience: latest values snapshot ────────────────────────────────── */
typedef struct {
    /* Moving averages */
    double sma_20;
    double sma_50;
    double sma_200;
    double ema_12;
    double ema_26;

    /* RSI */
    double rsi_14;

    /* MACD */
    double macd_line;
    double macd_signal;
    double macd_histogram;

    /* Bollinger Bands */
    double bb_upper;
    double bb_middle;
    double bb_lower;
    double bb_width;

    /* ATR */
    double atr_14;

    /* VWAP */
    double vwap;

    /* ADX */
    double adx_14;
    double plus_di;
    double minus_di;

    /* Keltner Channels */
    double kc_upper;
    double kc_middle;
    double kc_lower;

    /* Donchian Channels */
    double dc_upper;
    double dc_lower;
    double dc_middle;

    /* Stochastic RSI */
    double stoch_rsi_k;
    double stoch_rsi_d;

    /* CCI */
    double cci_20;

    /* Williams %R */
    double williams_r;

    /* OBV */
    double obv;
    double obv_sma;

    /* Ichimoku */
    double ichi_tenkan;
    double ichi_kijun;
    double ichi_senkou_a;
    double ichi_senkou_b;
    double ichi_chikou;

    /* Derived signals */
    bool   above_sma_200;       /* price > SMA200 (bullish) */
    bool   golden_cross;        /* SMA50 > SMA200 */
    bool   rsi_oversold;        /* RSI < 30 */
    bool   rsi_overbought;      /* RSI > 70 */
    bool   bb_squeeze;          /* BB width < 0.03 (low volatility) */
    bool   macd_bullish_cross;  /* MACD line > signal & histogram > 0 */
    bool   adx_trending;        /* ADX > 25 */
    bool   kc_squeeze;          /* BB inside KC */
    bool   ichi_bullish;        /* price > cloud + tenkan > kijun */

    bool   valid;
} tb_indicators_snapshot_t;

/* Compute all indicators from candle array and return latest values.
   Needs at least 200 candles for SMA200. Works with fewer but some fields invalid. */
tb_indicators_snapshot_t tb_indicators_compute(const tb_candle_input_t *candles,
                                                int n_candles);

#endif /* TB_INDICATORS_H */
