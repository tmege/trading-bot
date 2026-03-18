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

/* ── CMF (Chaikin Money Flow) ──────────────────────────────────────────── */

/* CMF over `period` candles (typically 20). Bounded [-1, +1].
   out[] has n_candles values. */
int tb_cmf(const tb_candle_input_t *candles, int n_candles,
           int period, double *out);

/* ── MFI (Money Flow Index) ───────────────────────────────────────────── */

/* MFI over `period` candles (typically 14). Range [0, 100].
   Volume-weighted RSI. out[] has n_candles values. */
int tb_mfi(const tb_candle_input_t *candles, int n_candles,
           int period, double *out);

/* ── Squeeze Momentum (LazyBear) ──────────────────────────────────────── */
typedef struct {
    double momentum;    /* momentum histogram value */
    bool   squeeze_on;  /* BB inside KC = squeeze active */
} tb_squeeze_val_t;

/* Squeeze Momentum: BB(20,2.0) vs KC(20,1.5).
   Momentum = linear regression residual of (close - midline).
   out[] has n_candles values. */
int tb_squeeze_momentum(const tb_candle_input_t *candles, int n_candles,
                        tb_squeeze_val_t *out);

/* ── ROC (Rate of Change) ──────────────────────────────────────────────── */

/* ROC = (close - close[n-period]) / close[n-period] * 100.
   out[] has n_candles values; first `period` are 0. */
int tb_roc(const tb_candle_input_t *candles, int n_candles,
           int period, double *out);

/* ── Z-Score ──────────────────────────────────────────────────────────── */

/* Z-Score = (close - SMA(period)) / stddev(period).
   out[] has n_candles values; first (period-1) are 0. */
int tb_zscore(const tb_candle_input_t *candles, int n_candles,
              int period, double *out);

/* ── FVG (Fair Value Gap) ─────────────────────────────────────────────── */
typedef struct {
    bool   bullish_fvg;  /* candle[i-2].high < candle[i].low */
    bool   bearish_fvg;  /* candle[i-2].low > candle[i].high */
    double fvg_size;     /* gap size as % of price */
} tb_fvg_val_t;

/* FVG detection: 3-candle gap patterns.
   out[] has n_candles values; first 2 are zero. */
int tb_fvg(const tb_candle_input_t *candles, int n_candles,
           tb_fvg_val_t *out);

/* ── Supertrend ───────────────────────────────────────────────────────── */
typedef struct {
    double value;       /* supertrend line value */
    bool   is_uptrend;  /* true if price above supertrend */
} tb_supertrend_val_t;

/* Supertrend: ATR-based trend indicator.
   Default: atr_period=10, multiplier=3.0.
   out[] has n_candles values. */
int tb_supertrend(const tb_candle_input_t *candles, int n_candles,
                  int atr_period, double multiplier,
                  tb_supertrend_val_t *out);

/* ── Parabolic SAR ────────────────────────────────────────────────────── */
typedef struct {
    double sar;         /* SAR value */
    bool   is_uptrend;  /* true if SAR below price (bullish) */
} tb_psar_val_t;

/* Parabolic SAR (Wilder).
   Default: af_start=0.02, af_max=0.20, af_step=0.02.
   out[] has n_candles values. */
int tb_psar(const tb_candle_input_t *candles, int n_candles,
            double af_start, double af_max, double af_step,
            tb_psar_val_t *out);

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

    /* CMF */
    double cmf_20;

    /* MFI */
    double mfi_14;

    /* Squeeze Momentum */
    double squeeze_mom;
    bool   squeeze_on;

    /* ROC */
    double roc_12;

    /* Z-Score */
    double zscore_20;

    /* FVG */
    bool   fvg_bull;
    bool   fvg_bear;
    double fvg_size;

    /* Supertrend */
    double supertrend;
    bool   supertrend_up;

    /* Parabolic SAR */
    double psar;
    bool   psar_up;

    /* Derived signals (original) */
    bool   above_sma_200;       /* price > SMA200 (bullish) */
    bool   golden_cross;        /* SMA50 > SMA200 */
    bool   rsi_oversold;        /* RSI < 30 */
    bool   rsi_overbought;      /* RSI > 70 */
    bool   bb_squeeze;          /* BB width < 0.03 (low volatility) */
    bool   macd_bullish_cross;  /* MACD line > signal & histogram > 0 */
    bool   adx_trending;        /* ADX > 25 */
    bool   kc_squeeze;          /* BB inside KC */
    bool   ichi_bullish;        /* price > cloud + tenkan > kijun */

    /* ── New derived indicators for data-driven strategies ──────────────── */

    /* Volatility percentile */
    double atr_pct;             /* ATR / close (raw %) */
    double atr_pct_rank;        /* percentile rank of atr_pct over last 100 bars [0..1] */

    /* Range percentile */
    double range_pct_rank;      /* percentile rank of (high-low)/close over last 100 bars */

    /* Volume ratio */
    double vol_ratio;           /* current volume / SMA20(volume) */

    /* Distance to moving averages (%) */
    double ema12_dist_pct;      /* (close - EMA12) / EMA12 * 100 */
    double sma20_dist_pct;      /* (close - SMA20) / SMA20 * 100 */

    /* Consecutive candle counts */
    int    consec_green;        /* consecutive green (close > open) candles ending at current */
    int    consec_red;          /* consecutive red (close < open) candles ending at current */

    /* Candle patterns */
    bool   bullish_engulf;      /* bullish engulfing pattern */
    bool   bearish_engulf;      /* bearish engulfing pattern */
    bool   shooting_star;       /* shooting star (upper wick > 2x body, small lower wick) */
    bool   hammer;              /* hammer (lower wick > 2x body, small upper wick) */
    bool   doji;                /* doji (body < 10% of total range) */

    /* MACD momentum direction */
    bool   macd_hist_incr;      /* histogram > prev histogram (accelerating) */
    bool   macd_hist_decr;      /* histogram < prev histogram (decelerating) */

    /* DI crossover state */
    bool   di_bull;             /* plus_di > minus_di */
    bool   di_bear;             /* minus_di > plus_di */

    /* RSI divergence (simplified: 10-bar lookback) */
    bool   rsi_bull_div;        /* price lower low but RSI higher low */
    bool   rsi_bear_div;        /* price higher high but RSI lower high */

    bool   valid;
} tb_indicators_snapshot_t;

/* Compute all indicators from candle array and return latest values.
   Needs at least 200 candles for SMA200. Works with fewer but some fields invalid. */
tb_indicators_snapshot_t tb_indicators_compute(const tb_candle_input_t *candles,
                                                int n_candles);

#endif /* TB_INDICATORS_H */
