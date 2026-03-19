/*
 * Regime Analyzer — C-native per-regime signal analysis
 *
 * Loads 5m candles, aggregates to TF, classifies market regime
 * (bull/bear/neutral), scans signal combos per-regime, runs
 * walk-forward validation and Monte Carlo simulation.
 *
 * Usage: ./regime_analyzer <coin> [n_days] [tf] [--validate] [--montecarlo]
 *
 * Reuses signal definitions and infrastructure from signal_scanner.c.
 * Output: markdown report to data/analysis/{COIN}_regime_report.md
 */

#include "strategy/indicators.h"
#include "core/logging.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

/* ── Config ────────────────────────────────────────────────────────────── */
#define CACHE_DB_PATH     "./data/candle_cache.db"
#define REPORT_DIR        "./data/analysis"
#define MAX_5M_CANDLES    2000000
#define MAX_TF_CANDLES    50000
#define FEE_ROUND_TRIP    0.0006   /* 0.06% maker+taker */
#define MAX_HOLD_BARS     24
#define MIN_OCCURRENCES   20
#define REGIME_HYSTERESIS 3        /* bars to confirm regime change */

/* TP/SL grid */
static const double TP_GRID[] = {1.0, 1.5, 2.0, 3.0, 4.0, 5.0};
static const double SL_GRID[] = {2.0, 3.0, 4.0, 5.0};
#define N_TP (sizeof(TP_GRID) / sizeof(TP_GRID[0]))
#define N_SL (sizeof(SL_GRID) / sizeof(SL_GRID[0]))

/* Walk-forward */
#define WF_N_SPLITS  5
#define WF_TRAIN_PCT 0.70

/* Monte Carlo */
#define MC_N_SIMS    10000
#define MC_SEED      42

/* ── Regime enum ──────────────────────────────────────────────────────── */
typedef enum {
    REGIME_BULL    = 0,
    REGIME_BEAR    = 1,
    REGIME_NEUTRAL = 2,
    REGIME_COUNT   = 3
} regime_t;

static const char *REGIME_NAMES[] = {"bull", "bear", "neutral"};

/* ── Signal definition (same as signal_scanner.c) ─────────────────────── */
typedef bool (*signal_fn)(const tb_indicators_snapshot_t *snap,
                          const tb_candle_input_t *candle);

typedef struct {
    const char *name;
    signal_fn   check;
} signal_def_t;

/* ── Signal implementations ──────────────────────────────────────────── */

/* RSI */
static bool sig_rsi_gt65(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->rsi_14 > 65; }
static bool sig_rsi_gt70(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->rsi_14 > 70; }
static bool sig_rsi_lt35(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->rsi_14 < 35; }
static bool sig_rsi_lt30(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->rsi_14 < 30; }
static bool sig_rsi_40_60(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->rsi_14 >= 40 && s->rsi_14 <= 60; }

/* Volatility */
static bool sig_low_vol(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { return c->close > 0 && (s->atr_14 / c->close) < 0.005; }
static bool sig_very_low_vol(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { return c->close > 0 && (s->atr_14 / c->close) < 0.003; }
static bool sig_high_vol(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { return c->close > 0 && (s->atr_14 / c->close) > 0.01; }
static bool sig_atr_p80(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->atr_pct_rank > 0.8; }
static bool sig_atr_p20(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->atr_pct_rank < 0.2; }

/* ADX */
static bool sig_adx_gt25(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->adx_14 > 25; }
static bool sig_adx_lt20(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->adx_14 < 20; }
static bool sig_adx_gt30(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->adx_14 > 30; }

/* DI */
static bool sig_di_bull(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->di_bull; }
static bool sig_di_bear(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->di_bear; }

/* MACD */
static bool sig_macd_gt0(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->macd_histogram > 0; }
static bool sig_macd_lt0(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->macd_histogram < 0; }
static bool sig_macd_accel(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->macd_hist_incr; }
static bool sig_macd_decel(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->macd_hist_decr; }
static bool sig_macd_bull_cross(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->macd_bullish_cross; }

/* BB */
static bool sig_bb_squeeze(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->bb_squeeze; }
static bool sig_above_bb_upper(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { return c->close > s->bb_upper && s->bb_upper > 0; }
static bool sig_below_bb_lower(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { return c->close < s->bb_lower && s->bb_lower > 0; }
static bool sig_below_bb_mid(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { return c->close < s->bb_middle && s->bb_middle > 0; }

/* SMA */
static bool sig_above_sma200(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->above_sma_200; }
static bool sig_below_sma200(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return !s->above_sma_200 && s->sma_200 > 0; }
static bool sig_golden_cross(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->golden_cross; }
static bool sig_death_cross(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return !s->golden_cross && s->sma_200 > 0 && s->sma_50 > 0; }
static bool sig_sma20_far_above(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->sma20_dist_pct > 2.0; }
static bool sig_sma20_far_below(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->sma20_dist_pct < -2.0; }

/* EMA */
static bool sig_ema12_far_above(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->ema12_dist_pct > 2.0; }
static bool sig_ema12_far_below(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->ema12_dist_pct < -2.0; }
static bool sig_ema_up(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->ema_12 > s->ema_26 && s->ema_26 > 0; }
static bool sig_ema_down(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->ema_12 < s->ema_26 && s->ema_26 > 0; }

/* Volume */
static bool sig_vol_spike_2x(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->vol_ratio > 2.0; }
static bool sig_vol_spike_3x(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->vol_ratio > 3.0; }
static bool sig_low_volume(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->vol_ratio < 0.5; }

/* OBV */
static bool sig_obv_above_sma(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->obv > s->obv_sma && s->obv_sma != 0; }
static bool sig_obv_below_sma(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->obv < s->obv_sma && s->obv_sma != 0; }

/* MFI */
static bool sig_mfi_gt70(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->mfi_14 > 70; }
static bool sig_mfi_lt20(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->mfi_14 < 20; }

/* CMF */
static bool sig_cmf_pos(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->cmf_20 > 0.05; }
static bool sig_cmf_neg(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->cmf_20 < -0.05; }

/* Squeeze */
static bool sig_squeeze_on(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->squeeze_on; }
static bool sig_squeeze_bull(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->squeeze_mom > 0; }
static bool sig_squeeze_bear(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->squeeze_mom < 0; }

/* Range */
static bool sig_range_wide(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->range_pct_rank > 0.8; }
static bool sig_range_narrow(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->range_pct_rank < 0.2; }

/* Candle patterns */
static bool sig_bullish_engulf(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->bullish_engulf; }
static bool sig_bearish_engulf(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->bearish_engulf; }
static bool sig_hammer(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->hammer; }
static bool sig_shooting_star(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->shooting_star; }
static bool sig_doji(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->doji; }

/* Consecutive */
static bool sig_consec_green_3(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->consec_green >= 3; }
static bool sig_consec_red_3(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->consec_red >= 3; }

/* Supertrend */
static bool sig_supertrend_bull(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->supertrend_up; }
static bool sig_supertrend_bear(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return !s->supertrend_up && s->supertrend > 0; }

/* PSAR */
static bool sig_psar_bull(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->psar_up; }
static bool sig_psar_bear(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return !s->psar_up && s->psar > 0; }

/* Ichimoku */
static bool sig_ichi_bullish(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->ichi_bullish; }

/* RSI divergence */
static bool sig_rsi_bull_div(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->rsi_bull_div; }
static bool sig_rsi_bear_div(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->rsi_bear_div; }

/* ROC */
static bool sig_roc_positive(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->roc_12 > 0; }
static bool sig_roc_negative(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->roc_12 < 0; }
static bool sig_roc_strong(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->roc_12 > 3.0 || s->roc_12 < -3.0; }

/* Z-Score */
static bool sig_zscore_high(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->zscore_20 > 2.0; }
static bool sig_zscore_low(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) { (void)c; return s->zscore_20 < -2.0; }

/* ── All signals array ──────────────────────────────────────────────────── */
static const signal_def_t ALL_SIGNALS[] = {
    {"rsi_gt65", sig_rsi_gt65}, {"rsi_gt70", sig_rsi_gt70},
    {"rsi_lt35", sig_rsi_lt35}, {"rsi_lt30", sig_rsi_lt30}, {"rsi_40_60", sig_rsi_40_60},
    {"low_vol", sig_low_vol}, {"very_low_vol", sig_very_low_vol}, {"high_vol", sig_high_vol},
    {"atr_p80", sig_atr_p80}, {"atr_p20", sig_atr_p20},
    {"adx_gt25", sig_adx_gt25}, {"adx_lt20", sig_adx_lt20}, {"adx_gt30", sig_adx_gt30},
    {"di_bull", sig_di_bull}, {"di_bear", sig_di_bear},
    {"macd_gt0", sig_macd_gt0}, {"macd_lt0", sig_macd_lt0},
    {"macd_accel", sig_macd_accel}, {"macd_decel", sig_macd_decel}, {"macd_bull_cross", sig_macd_bull_cross},
    {"bb_squeeze", sig_bb_squeeze}, {"above_bb_upper", sig_above_bb_upper},
    {"below_bb_lower", sig_below_bb_lower}, {"below_bb_mid", sig_below_bb_mid},
    {"above_sma200", sig_above_sma200}, {"below_sma200", sig_below_sma200},
    {"golden_cross", sig_golden_cross}, {"death_cross", sig_death_cross},
    {"sma20_far_above", sig_sma20_far_above}, {"sma20_far_below", sig_sma20_far_below},
    {"ema12_far_above", sig_ema12_far_above}, {"ema12_far_below", sig_ema12_far_below},
    {"ema_up", sig_ema_up}, {"ema_down", sig_ema_down},
    {"vol_spike_2x", sig_vol_spike_2x}, {"vol_spike_3x", sig_vol_spike_3x}, {"low_volume", sig_low_volume},
    {"obv_above_sma", sig_obv_above_sma}, {"obv_below_sma", sig_obv_below_sma},
    {"mfi_gt70", sig_mfi_gt70}, {"mfi_lt20", sig_mfi_lt20},
    {"cmf_pos", sig_cmf_pos}, {"cmf_neg", sig_cmf_neg},
    {"squeeze_on", sig_squeeze_on}, {"squeeze_bull", sig_squeeze_bull}, {"squeeze_bear", sig_squeeze_bear},
    {"range_wide", sig_range_wide}, {"range_narrow", sig_range_narrow},
    {"bullish_engulf", sig_bullish_engulf}, {"bearish_engulf", sig_bearish_engulf},
    {"hammer", sig_hammer}, {"shooting_star", sig_shooting_star}, {"doji", sig_doji},
    {"consec_green_3", sig_consec_green_3}, {"consec_red_3", sig_consec_red_3},
    {"supertrend_bull", sig_supertrend_bull}, {"supertrend_bear", sig_supertrend_bear},
    {"psar_bull", sig_psar_bull}, {"psar_bear", sig_psar_bear},
    {"ichi_bullish", sig_ichi_bullish},
    {"rsi_bull_div", sig_rsi_bull_div}, {"rsi_bear_div", sig_rsi_bear_div},
    {"roc_positive", sig_roc_positive}, {"roc_negative", sig_roc_negative}, {"roc_strong", sig_roc_strong},
    {"zscore_high", sig_zscore_high}, {"zscore_low", sig_zscore_low},
};
#define N_SIGNALS (sizeof(ALL_SIGNALS) / sizeof(ALL_SIGNALS[0]))

/* ── Load 5m candles from SQLite ────────────────────────────────────────── */
static int load_5m_candles(const char *coin, int n_days,
                            tb_candle_input_t *out, int max) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(CACHE_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "ERROR: cannot open %s\n", CACHE_DB_PATH);
        return -1;
    }

    int64_t now_s = (int64_t)time(NULL);
    int64_t end_ms = now_s * 1000;
    int64_t start_ms = (now_s - (int64_t)n_days * 86400) * 1000;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
        "SELECT time_ms, open, high, low, close, volume FROM candles "
        "WHERE coin=? AND interval='5m' AND time_ms>=? AND time_ms<=? "
        "ORDER BY time_ms ASC",
        -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, coin, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, start_ms);
    sqlite3_bind_int64(stmt, 3, end_ms);

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < max) {
        out[n].time_ms = sqlite3_column_int64(stmt, 0);
        out[n].open    = sqlite3_column_double(stmt, 1);
        out[n].high    = sqlite3_column_double(stmt, 2);
        out[n].low     = sqlite3_column_double(stmt, 3);
        out[n].close   = sqlite3_column_double(stmt, 4);
        out[n].volume  = sqlite3_column_double(stmt, 5);
        n++;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return n;
}

/* ── Aggregate 5m → TF ──────────────────────────────────────────────────── */
static int aggregate_to_tf(const tb_candle_input_t *m5, int n5,
                            int64_t tf_ms, tb_candle_input_t *out, int max) {
    if (n5 < 2) return 0;
    int bars_per_tf = (int)(tf_ms / 300000LL);
    if (bars_per_tf < 1) bars_per_tf = 1;

    int n = 0, i = 0;

    /* Align to TF boundary */
    while (i < n5 && (m5[i].time_ms % tf_ms) != 0)
        i++;

    while (i + bars_per_tf - 1 < n5 && n < max) {
        int64_t tf_start = m5[i].time_ms;
        /* Verify all sub-candles belong to same TF period */
        if (m5[i + bars_per_tf - 1].time_ms != tf_start + (int64_t)(bars_per_tf - 1) * 300000LL) {
            i++;
            while (i < n5 && (m5[i].time_ms % tf_ms) != 0) i++;
            continue;
        }

        out[n].time_ms = tf_start;
        out[n].open    = m5[i].open;
        out[n].close   = m5[i + bars_per_tf - 1].close;
        out[n].high    = m5[i].high;
        out[n].low     = m5[i].low;
        out[n].volume  = 0;

        for (int j = 0; j < bars_per_tf; j++) {
            if (m5[i + j].high > out[n].high) out[n].high = m5[i + j].high;
            if (m5[i + j].low  < out[n].low)  out[n].low  = m5[i + j].low;
            out[n].volume += m5[i + j].volume;
        }

        n++;
        i += bars_per_tf;
    }

    return n;
}

/* ── Regime classification ─────────────────────────────────────────────── */
static void classify_regime(const tb_candle_input_t *bars,
                             const tb_indicators_snapshot_t *snaps,
                             int n_bars, regime_t *out) {
    regime_t current = REGIME_NEUTRAL;
    int confirm_count = 0;
    regime_t pending = REGIME_NEUTRAL;

    for (int i = 0; i < n_bars; i++) {
        if (!snaps[i].valid || snaps[i].sma_200 <= 0) {
            out[i] = current;
            continue;
        }

        /* Determine raw regime */
        regime_t raw;
        bool above_sma200 = bars[i].close > snaps[i].sma_200;
        bool ema_bull = snaps[i].ema_12 > snaps[i].ema_26 && snaps[i].ema_26 > 0;
        bool trending = snaps[i].adx_14 > 20;
        bool di_bull_raw = snaps[i].plus_di > snaps[i].minus_di;

        if (above_sma200 && ema_bull && trending && di_bull_raw)
            raw = REGIME_BULL;
        else if (!above_sma200 && !ema_bull && trending && !di_bull_raw)
            raw = REGIME_BEAR;
        else
            raw = REGIME_NEUTRAL;

        /* Hysteresis: require N consecutive bars to confirm regime change */
        if (raw != current) {
            if (raw == pending) {
                confirm_count++;
                if (confirm_count >= REGIME_HYSTERESIS) {
                    current = raw;
                    confirm_count = 0;
                }
            } else {
                pending = raw;
                confirm_count = 1;
            }
        } else {
            confirm_count = 0;
            pending = current;
        }

        out[i] = current;
    }
}

/* ── Regime stats ──────────────────────────────────────────────────────── */
typedef struct {
    int    count;
    double pct;
    double avg_duration;
    double avg_volatility;
} regime_stat_t;

static void compute_regime_stats(const regime_t *regimes, int n,
                                   const tb_candle_input_t *bars,
                                   const tb_indicators_snapshot_t *snaps,
                                   regime_stat_t *out) {
    for (int r = 0; r < REGIME_COUNT; r++) {
        out[r].count = 0;
        out[r].avg_volatility = 0;
    }

    /* Count and volatility */
    for (int i = 0; i < n; i++) {
        int r = (int)regimes[i];
        out[r].count++;
        if (bars[i].close > 0 && snaps[i].valid)
            out[r].avg_volatility += snaps[i].atr_14 / bars[i].close * 100.0;
    }

    for (int r = 0; r < REGIME_COUNT; r++) {
        out[r].pct = n > 0 ? (double)out[r].count / n * 100.0 : 0;
        if (out[r].count > 0)
            out[r].avg_volatility /= out[r].count;
    }

    /* Average duration (consecutive runs) */
    for (int r = 0; r < REGIME_COUNT; r++) {
        int n_runs = 0;
        int total_dur = 0;
        int run_len = 0;
        for (int i = 0; i < n; i++) {
            if ((int)regimes[i] == r) {
                run_len++;
            } else if (run_len > 0) {
                n_runs++;
                total_dur += run_len;
                run_len = 0;
            }
        }
        if (run_len > 0) { n_runs++; total_dur += run_len; }
        out[r].avg_duration = n_runs > 0 ? (double)total_dur / n_runs : 0;
    }
}

/* ── Regime transition matrix ──────────────────────────────────────────── */
static void compute_transition_matrix(const regime_t *regimes, int n,
                                        double matrix[REGIME_COUNT][REGIME_COUNT]) {
    int counts[REGIME_COUNT][REGIME_COUNT] = {{0}};
    int row_totals[REGIME_COUNT] = {0};

    for (int i = 0; i < n - 1; i++) {
        int from = (int)regimes[i];
        int to   = (int)regimes[i + 1];
        counts[from][to]++;
        row_totals[from]++;
    }

    for (int r = 0; r < REGIME_COUNT; r++) {
        for (int c = 0; c < REGIME_COUNT; c++) {
            matrix[r][c] = row_totals[r] > 0 ?
                (double)counts[r][c] / row_totals[r] : 0;
        }
    }
}

/* ── Single-position trade simulation ───────────────────────────────────── */
typedef struct {
    int    total_trades;
    int    wins;
    double total_pnl;
    double sum_sq_ret;
    double sum_neg_sq_ret;
    double max_drawdown;
    /* Per-trade PnL storage for Monte Carlo */
    double *trade_pnls;
    int     trade_cap;
} sim_result_t;

static sim_result_t simulate_single_pos(const tb_candle_input_t *candles, int n,
                                          const bool *signal_active,
                                          int direction,
                                          double tp_pct, double sl_pct,
                                          bool store_pnls) {
    sim_result_t r = {0};
    if (store_pnls) {
        r.trade_cap = 1024;
        r.trade_pnls = malloc((size_t)r.trade_cap * sizeof(double));
    }

    double equity = 100.0;
    double peak = equity;

    int i = 0;
    while (i < n) {
        if (!signal_active[i]) { i++; continue; }

        double entry = candles[i].close;
        if (entry <= 0) { i++; continue; }

        double tp_price, sl_price;
        if (direction == 1) {
            tp_price = entry * (1.0 + tp_pct / 100.0);
            sl_price = entry * (1.0 - sl_pct / 100.0);
        } else {
            tp_price = entry * (1.0 - tp_pct / 100.0);
            sl_price = entry * (1.0 + sl_pct / 100.0);
        }

        double exit_price = 0;
        int bars_held = 0;

        for (int j = i + 1; j < n && bars_held < MAX_HOLD_BARS; j++) {
            bars_held++;
            if (direction == 1) {
                if (candles[j].low <= sl_price)  { exit_price = sl_price; break; }
                if (candles[j].high >= tp_price) { exit_price = tp_price; break; }
            } else {
                if (candles[j].high >= sl_price) { exit_price = sl_price; break; }
                if (candles[j].low <= tp_price)  { exit_price = tp_price; break; }
            }
            if (bars_held >= MAX_HOLD_BARS) { exit_price = candles[j].close; break; }
        }

        if (exit_price <= 0) { i++; continue; }

        double pnl_pct;
        if (direction == 1)
            pnl_pct = (exit_price - entry) / entry * 100.0 - FEE_ROUND_TRIP * 100.0;
        else
            pnl_pct = (entry - exit_price) / entry * 100.0 - FEE_ROUND_TRIP * 100.0;

        r.total_trades++;
        r.total_pnl += pnl_pct;
        if (pnl_pct > 0) r.wins++;

        double ret = pnl_pct / 100.0;
        r.sum_sq_ret += ret * ret;
        if (ret < 0) r.sum_neg_sq_ret += ret * ret;

        equity *= (1.0 + ret);
        if (equity > peak) peak = equity;
        double dd = (peak - equity) / peak * 100.0;
        if (dd > r.max_drawdown) r.max_drawdown = dd;

        /* Store trade PnL for MC */
        if (store_pnls && r.trade_pnls) {
            if (r.total_trades > r.trade_cap) {
                r.trade_cap *= 2;
                double *tmp = realloc(r.trade_pnls, (size_t)r.trade_cap * sizeof(double));
                if (tmp) r.trade_pnls = tmp;
            }
            r.trade_pnls[r.total_trades - 1] = pnl_pct;
        }

        i += bars_held + 1;
    }

    return r;
}

static void sim_result_free(sim_result_t *r) {
    free(r->trade_pnls);
    r->trade_pnls = NULL;
}

/* ── Stats helpers ──────────────────────────────────────────────────────── */
static double compute_sharpe(const sim_result_t *r) {
    if (r->total_trades < 2) return 0;
    double mean = (r->total_pnl / 100.0) / r->total_trades;
    double var = r->sum_sq_ret / r->total_trades - mean * mean;
    double std = sqrt(var > 0 ? var : 0);
    if (std < 1e-10) return 0;
    return (mean / std) * sqrt(208.0);
}

static double compute_ev(const sim_result_t *r) {
    if (r->total_trades == 0) return 0;
    return r->total_pnl / r->total_trades;
}

static double compute_wr(const sim_result_t *r) {
    if (r->total_trades == 0) return 0;
    return (double)r->wins / r->total_trades * 100.0;
}

static double compute_pf(const sim_result_t *r) {
    if (r->total_trades == 0) return 0;
    double gross_profit = 0, gross_loss = 0;
    /* Approximate from total_pnl and win/loss counts */
    double avg_win = r->wins > 0 ? r->total_pnl / r->wins : 0; /* rough */
    /* Can't compute exactly without individual trades, use EV-based approx */
    double ev = compute_ev(r);
    double wr = compute_wr(r) / 100.0;
    if (wr <= 0 || wr >= 1) return ev > 0 ? 9999.0 : 0;
    double avg_w = ev / wr;  /* rough approximation */
    double avg_l = -ev / (1.0 - wr);
    (void)avg_win;
    (void)gross_profit;
    (void)gross_loss;
    if (avg_l == 0) return 9999.0;
    return fabs(avg_w * wr / (avg_l * (1.0 - wr)));
}

/* Wilson confidence interval lower bound */
static double wilson_ci_lower(int successes, int total) {
    if (total == 0) return 0;
    double z = 1.96; /* 95% CI */
    double p = (double)successes / total;
    double n = (double)total;
    double denom = 1.0 + z * z / n;
    double center = p + z * z / (2.0 * n);
    double margin = z * sqrt((p * (1.0 - p) + z * z / (4.0 * n)) / n);
    return (center - margin) / denom;
}

/* ── xoshiro256** PRNG ─────────────────────────────────────────────────── */
static uint64_t xo_s[4];

static uint64_t xo_rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static void xo_seed(uint64_t seed) {
    /* SplitMix64 to initialize state */
    for (int i = 0; i < 4; i++) {
        seed += 0x9e3779b97f4a7c15ULL;
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        xo_s[i] = z ^ (z >> 31);
    }
}

static uint64_t xo_next(void) {
    uint64_t result = xo_rotl(xo_s[1] * 5, 7) * 9;
    uint64_t t = xo_s[1] << 17;
    xo_s[2] ^= xo_s[0];
    xo_s[3] ^= xo_s[1];
    xo_s[1] ^= xo_s[2];
    xo_s[0] ^= xo_s[3];
    xo_s[2] ^= t;
    xo_s[3] = xo_rotl(xo_s[3], 45);
    return result;
}

/* Random int in [0, max) */
static int xo_rand_int(int max) {
    return (int)(xo_next() % (uint64_t)max);
}

/* ── Monte Carlo bootstrap ─────────────────────────────────────────────── */
typedef struct {
    bool   valid;
    int    n_sims;
    double p_ruin;
    double p95_drawdown;
    double p99_drawdown;
    double median_return;
    double p5_return;
    double p95_return;
    double median_final_equity;
} mc_result_t;

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static double percentile(const double *sorted, int n, double p) {
    if (n <= 0) return 0;
    double idx = p / 100.0 * (n - 1);
    int lo = (int)idx;
    int hi = lo + 1;
    if (hi >= n) return sorted[n - 1];
    double frac = idx - lo;
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

static mc_result_t monte_carlo_bootstrap(const double *trade_pnls, int n_trades,
                                           double capital, double leverage,
                                           int n_sims) {
    mc_result_t mc = {0};
    if (n_trades < 10) return mc;

    double *final_returns = malloc((size_t)n_sims * sizeof(double));
    double *max_dds       = malloc((size_t)n_sims * sizeof(double));
    if (!final_returns || !max_dds) {
        free(final_returns); free(max_dds);
        return mc;
    }

    int ruin_count = 0;
    double ruin_threshold = capital * 0.5; /* 50% loss = ruin */

    for (int sim = 0; sim < n_sims; sim++) {
        double equity = capital;
        double peak = capital;
        double max_dd = 0;
        bool ruined = false;

        /* Bootstrap: sample n_trades with replacement */
        for (int t = 0; t < n_trades; t++) {
            int idx = xo_rand_int(n_trades);
            double pnl_pct = trade_pnls[idx];
            double trade_pnl = equity * leverage * (pnl_pct / 100.0);
            equity += trade_pnl;

            if (equity > peak) peak = equity;
            double dd = (peak - equity) / peak * 100.0;
            if (dd > max_dd) max_dd = dd;

            if (equity < ruin_threshold) {
                ruined = true;
                break;
            }
        }

        if (ruined) ruin_count++;
        final_returns[sim] = (equity - capital) / capital * 100.0;
        max_dds[sim] = max_dd;
    }

    /* Sort for percentiles */
    qsort(final_returns, (size_t)n_sims, sizeof(double), cmp_double);
    qsort(max_dds, (size_t)n_sims, sizeof(double), cmp_double);

    mc.valid = true;
    mc.n_sims = n_sims;
    mc.p_ruin = (double)ruin_count / n_sims * 100.0;
    mc.p95_drawdown = percentile(max_dds, n_sims, 95.0);
    mc.p99_drawdown = percentile(max_dds, n_sims, 99.0);
    mc.median_return = percentile(final_returns, n_sims, 50.0);
    mc.p5_return = percentile(final_returns, n_sims, 5.0);
    mc.p95_return = percentile(final_returns, n_sims, 95.0);
    mc.median_final_equity = capital * (1.0 + mc.median_return / 100.0);

    free(final_returns);
    free(max_dds);
    return mc;
}

/* ── Walk-forward validation ───────────────────────────────────────────── */
typedef struct {
    bool   valid;
    bool   robust;
    double is_ev;
    double is_wr;
    double oos_ev;
    double oos_wr;
    double degradation_pct;
} wf_result_t;

static wf_result_t walk_forward_validate(const tb_candle_input_t *bars, int n_bars,
                                            const bool *signal_active,
                                            int direction,
                                            double tp_pct, double sl_pct) {
    wf_result_t wf = {0};

    if (n_bars < 200) return wf;

    /* Count total signals */
    int total_signals = 0;
    for (int i = 0; i < n_bars; i++) {
        if (signal_active[i]) total_signals++;
    }
    if (total_signals < 30) return wf;

    /* 5 overlapping splits, each covering ~60% of data */
    double sum_is_ev = 0, sum_oos_ev = 0;
    double sum_is_wr = 0, sum_oos_wr = 0;
    int valid_splits = 0;

    int window = (int)(n_bars * 0.6);
    int step = (n_bars - window) / (WF_N_SPLITS - 1);
    if (step < 1) step = 1;

    for (int split = 0; split < WF_N_SPLITS; split++) {
        int start = split * step;
        int end = start + window;
        if (end > n_bars) end = n_bars;
        int len = end - start;
        if (len < 100) continue;

        int train_end = start + (int)(len * WF_TRAIN_PCT);
        int train_len = train_end - start;
        int test_len = end - train_end;
        if (train_len < 50 || test_len < 20) continue;

        /* Train (IS) */
        sim_result_t r_is = simulate_single_pos(
            &bars[start], train_len, &signal_active[start],
            direction, tp_pct, sl_pct, false);

        if (r_is.total_trades < 10) continue;

        /* Test (OOS) */
        sim_result_t r_oos = simulate_single_pos(
            &bars[train_end], test_len, &signal_active[train_end],
            direction, tp_pct, sl_pct, false);

        if (r_oos.total_trades < 5) continue;

        sum_is_ev += compute_ev(&r_is);
        sum_oos_ev += compute_ev(&r_oos);
        sum_is_wr += compute_wr(&r_is);
        sum_oos_wr += compute_wr(&r_oos);
        valid_splits++;
    }

    if (valid_splits < 2) return wf;

    wf.valid = true;
    wf.is_ev = sum_is_ev / valid_splits;
    wf.oos_ev = sum_oos_ev / valid_splits;
    wf.is_wr = sum_is_wr / valid_splits;
    wf.oos_wr = sum_oos_wr / valid_splits;
    wf.degradation_pct = wf.is_ev > 0 ?
        (wf.is_ev - wf.oos_ev) / wf.is_ev * 100.0 : 0;
    wf.robust = wf.degradation_pct < 30.0 && wf.oos_ev > 0;

    return wf;
}

/* ── Result entry for per-regime results ─────────────────────────────────── */
typedef struct {
    char    combo_name[256];
    char    direction[8];
    int     regime;
    double  tp, sl;
    int     count;
    double  wr;
    double  ev;
    double  sharpe;
    double  pf;
    double  ci_lower;
    /* Walk-forward (optional) */
    bool    wf_valid;
    bool    wf_robust;
    double  wf_oos_ev;
    double  wf_degradation;
    /* Trade PnLs for MC */
    double *trade_pnls;
    int     n_trades;
} regime_result_t;

static int cmp_by_ev(const void *a, const void *b) {
    double ea = ((const regime_result_t *)a)->ev;
    double eb = ((const regime_result_t *)b)->ev;
    if (eb > ea) return 1;
    if (eb < ea) return -1;
    return 0;
}

/* ── Generate markdown report ──────────────────────────────────────────── */
static void generate_report(const char *coin, const char *tf, int n_bars,
                              const regime_stat_t *rstats,
                              const double trans[REGIME_COUNT][REGIME_COUNT],
                              regime_result_t *results, int n_results,
                              const mc_result_t *mc_global,
                              const char *date_range) {
    mkdir(REPORT_DIR, 0755);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%s_regime_report.md", REPORT_DIR, coin, tf);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "ERROR: cannot create %s\n", path);
        return;
    }

    /* Get current date */
    time_t now = time(NULL);
    char date_buf[32];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M", localtime(&now));

    fprintf(f, "# Analyse Per-Regime — %s %s\n\n", coin, tf);
    fprintf(f, "*Genere le %s*\n\n", date_buf);
    fprintf(f, "- **Coin** : %s\n", coin);
    fprintf(f, "- **Timeframe** : %s\n", tf);
    fprintf(f, "- **Bougies analysees** : %d\n", n_bars);
    fprintf(f, "- **Periode** : %s\n", date_range);
    fprintf(f, "- **Fees** : 0.06%% round-trip\n\n");

    /* Regime distribution */
    fprintf(f, "## Distribution des Regimes\n\n");
    fprintf(f, "| Regime | Bougies | %% | Duree moy | Vol moy |\n");
    fprintf(f, "|--------|---------|---|-----------|----------|\n");
    for (int r = 0; r < REGIME_COUNT; r++) {
        fprintf(f, "| %s | %d | %.1f%% | %.0f bars | %.3f%% |\n",
                REGIME_NAMES[r], rstats[r].count, rstats[r].pct,
                rstats[r].avg_duration, rstats[r].avg_volatility);
    }
    fprintf(f, "\n");

    /* Transition matrix */
    fprintf(f, "## Matrice de Transition\n\n");
    fprintf(f, "| De \\ Vers | bull | bear | neutral |\n");
    fprintf(f, "|-----------|------|------|----------|\n");
    for (int r = 0; r < REGIME_COUNT; r++) {
        fprintf(f, "| %s | %.3f | %.3f | %.3f |\n",
                REGIME_NAMES[r], trans[r][0], trans[r][1], trans[r][2]);
    }
    fprintf(f, "\n");

    /* Top signals per regime × direction */
    for (int regime = 0; regime < REGIME_COUNT; regime++) {
        for (int dir = 0; dir < 2; dir++) {
            const char *dir_str = dir == 0 ? "LONG" : "SHORT";
            int dir_val = dir == 0 ? 1 : -1;

            /* Collect matching results */
            int match_indices[500];
            int n_match = 0;
            for (int i = 0; i < n_results && n_match < 500; i++) {
                if (results[i].regime == regime &&
                    ((dir_val == 1 && strcmp(results[i].direction, "LONG") == 0) ||
                     (dir_val == -1 && strcmp(results[i].direction, "SHORT") == 0))) {
                    match_indices[n_match++] = i;
                }
            }

            if (n_match == 0) continue;

            fprintf(f, "## Top Signaux — %s / %s\n\n",
                    REGIME_NAMES[regime], dir_str);
            fprintf(f, "| # | Signal | TP/SL | Count | WR%% | EV%% | PF | Sharpe | CI_low |\n");
            fprintf(f, "|---|--------|-------|-------|------|------|-------|--------|--------|\n");

            int limit = n_match < 10 ? n_match : 10;
            for (int k = 0; k < limit; k++) {
                regime_result_t *r = &results[match_indices[k]];
                fprintf(f, "| %d | `%s` | %.0f/%.0f | %d | %.1f | %.3f | %.2f | %.2f | %.1f%% |\n",
                        k + 1, r->combo_name, r->tp, r->sl,
                        r->count, r->wr, r->ev,
                        r->pf > 9999 ? 9999.0 : r->pf,
                        r->sharpe, r->ci_lower * 100.0);
            }
            fprintf(f, "\n");
        }
    }

    /* Walk-forward results */
    bool has_wf = false;
    for (int i = 0; i < n_results; i++) {
        if (results[i].wf_valid) { has_wf = true; break; }
    }
    if (has_wf) {
        fprintf(f, "## Walk-Forward Validation\n\n");
        fprintf(f, "| Signal | Regime | Dir | OOS EV%% | Degradation | Statut |\n");
        fprintf(f, "|--------|--------|-----|---------|-------------|--------|\n");
        for (int i = 0; i < n_results; i++) {
            if (!results[i].wf_valid) continue;
            fprintf(f, "| `%s` | %s | %s | %.3f | %.0f%% | %s |\n",
                    results[i].combo_name,
                    REGIME_NAMES[results[i].regime],
                    results[i].direction,
                    results[i].wf_oos_ev,
                    results[i].wf_degradation,
                    results[i].wf_robust ? "ROBUST" : "FRAGILE");
        }
        fprintf(f, "\n");
    }

    /* Monte Carlo */
    if (mc_global && mc_global->valid) {
        fprintf(f, "## Monte Carlo Simulation\n\n");
        fprintf(f, "- **Simulations** : %d\n", mc_global->n_sims);
        fprintf(f, "- **P(ruine)** : %.2f%%\n", mc_global->p_ruin);
        fprintf(f, "- **P95 Drawdown** : %.1f%%\n", mc_global->p95_drawdown);
        fprintf(f, "- **P99 Drawdown** : %.1f%%\n", mc_global->p99_drawdown);
        fprintf(f, "- **Rendement median** : %.1f%%\n", mc_global->median_return);
        fprintf(f, "- **Rendement P5/P95** : %.1f%% / %.1f%%\n",
                mc_global->p5_return, mc_global->p95_return);
        fprintf(f, "- **Equity finale mediane** : $%.0f\n\n", mc_global->median_final_equity);
    }

    fclose(f);
    fprintf(stderr, "  Report: %s\n", path);
}

/* ── Main ──────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    tb_log_init("./logs", TB_LOG_LVL_WARN);

    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <coin> [n_days] [tf] [--validate] [--montecarlo]\n"
            "  coin:      BTC, ETH, SOL, DOGE\n"
            "  n_days:    history length (default 2000)\n"
            "  tf:        1h, 4h (default 1h)\n"
            "  --validate:    enable walk-forward validation\n"
            "  --montecarlo:  enable Monte Carlo simulation\n",
            argv[0]);
        return 1;
    }

    const char *coin = argv[1];
    int n_days = 2000;
    const char *tf = "1h";
    bool do_validate = false;
    bool do_montecarlo = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--validate") == 0) do_validate = true;
        else if (strcmp(argv[i], "--montecarlo") == 0) do_montecarlo = true;
        else if (argv[i][0] != '-' && n_days == 2000 && atoi(argv[i]) > 0) n_days = atoi(argv[i]);
        else if (argv[i][0] != '-') tf = argv[i];
    }

    if (n_days < 30) n_days = 30;
    if (n_days > 5000) n_days = 5000;

    /* Parse TF → ms */
    int64_t tf_ms = 3600000LL;
    if (strcmp(tf, "4h") == 0) tf_ms = 14400000LL;
    else if (strcmp(tf, "1d") == 0) tf_ms = 86400000LL;

    fprintf(stderr, "======================================================================\n");
    fprintf(stderr, "  REGIME ANALYZER : %s %s (%d days)\n", coin, tf, n_days);
    fprintf(stderr, "  Validate=%s, MonteCarlo=%s\n",
            do_validate ? "yes" : "no", do_montecarlo ? "yes" : "no");
    fprintf(stderr, "======================================================================\n");

    /* ── 1. Load 5m candles ─────────────────────────────────────────────── */
    fprintf(stderr, "\n[1/7] Loading 5m candles...");
    tb_candle_input_t *m5 = calloc(MAX_5M_CANDLES, sizeof(tb_candle_input_t));
    if (!m5) { fprintf(stderr, " malloc failed\n"); return 1; }

    int n5 = load_5m_candles(coin, n_days, m5, MAX_5M_CANDLES);
    if (n5 < 1000) {
        fprintf(stderr, " only %d candles, need >= 1000\n", n5);
        free(m5);
        return 1;
    }
    fprintf(stderr, " %d candles\n", n5);

    /* Date range */
    char date_range[128];
    {
        time_t t1 = (time_t)(m5[0].time_ms / 1000);
        time_t t2 = (time_t)(m5[n5-1].time_ms / 1000);
        char s1[32], s2[32];
        strftime(s1, sizeof(s1), "%Y-%m-%d", gmtime(&t1));
        strftime(s2, sizeof(s2), "%Y-%m-%d", gmtime(&t2));
        snprintf(date_range, sizeof(date_range), "%s -> %s", s1, s2);
    }

    /* ── 2. Aggregate to TF ─────────────────────────────────────────────── */
    fprintf(stderr, "[2/7] Aggregating 5m -> %s...", tf);
    tb_candle_input_t *bars_all = calloc(MAX_TF_CANDLES, sizeof(tb_candle_input_t));
    if (!bars_all) { free(m5); return 1; }

    int n_tf = aggregate_to_tf(m5, n5, tf_ms, bars_all, MAX_TF_CANDLES);
    free(m5);

    if (n_tf < 500) {
        fprintf(stderr, " only %d bars, need >= 500\n", n_tf);
        free(bars_all);
        return 1;
    }
    fprintf(stderr, " %d bars (%.1f years)\n", n_tf, n_tf / 8760.0);

    /* ── 3. Compute indicators ──────────────────────────────────────────── */
    fprintf(stderr, "[3/7] Computing indicators...");

    int window = 250;
    int n_bars = n_tf - window;
    if (n_bars < 200) {
        fprintf(stderr, " not enough bars for warmup\n");
        free(bars_all);
        return 1;
    }

    tb_indicators_snapshot_t *snaps = calloc((size_t)n_bars, sizeof(tb_indicators_snapshot_t));
    if (!snaps) { free(bars_all); return 1; }

    for (int i = 0; i < n_bars; i++) {
        snaps[i] = tb_indicators_compute(&bars_all[i], window + 1);
    }

    tb_candle_input_t *bars = &bars_all[window];
    fprintf(stderr, " %d valid bars\n", n_bars);

    /* ── 4. Classify regime ─────────────────────────────────────────────── */
    fprintf(stderr, "[4/7] Classifying regimes...");

    regime_t *regimes = calloc((size_t)n_bars, sizeof(regime_t));
    if (!regimes) { free(snaps); free(bars_all); return 1; }

    classify_regime(bars, snaps, n_bars, regimes);

    regime_stat_t rstats[REGIME_COUNT];
    compute_regime_stats(regimes, n_bars, bars, snaps, rstats);

    double trans_matrix[REGIME_COUNT][REGIME_COUNT];
    compute_transition_matrix(regimes, n_bars, trans_matrix);

    fprintf(stderr, " done\n");
    fprintf(stderr, "  Regime distribution:\n");
    for (int r = 0; r < REGIME_COUNT; r++) {
        fprintf(stderr, "    %-8s: %5d bars (%5.1f%%), avg dur=%.0f bars, vol=%.3f%%\n",
                REGIME_NAMES[r], rstats[r].count, rstats[r].pct,
                rstats[r].avg_duration, rstats[r].avg_volatility);
    }

    fprintf(stderr, "  Transition matrix:\n");
    fprintf(stderr, "    %10s -> bull    -> bear    -> neutral\n", "");
    for (int r = 0; r < REGIME_COUNT; r++) {
        fprintf(stderr, "    %10s   %.3f    %.3f    %.3f\n",
                REGIME_NAMES[r], trans_matrix[r][0], trans_matrix[r][1], trans_matrix[r][2]);
    }

    /* ── 5. Pre-compute signals ─────────────────────────────────────────── */
    fprintf(stderr, "[5/7] Computing signals...");

    bool **sig_active = calloc(N_SIGNALS, sizeof(bool *));
    for (int s = 0; s < (int)N_SIGNALS; s++) {
        sig_active[s] = calloc((size_t)n_bars, sizeof(bool));
        for (int i = 0; i < n_bars; i++) {
            if (snaps[i].valid)
                sig_active[s][i] = ALL_SIGNALS[s].check(&snaps[i], &bars[i]);
        }
    }

    /* Regime masks */
    bool *regime_masks[REGIME_COUNT];
    for (int r = 0; r < REGIME_COUNT; r++) {
        regime_masks[r] = calloc((size_t)n_bars, sizeof(bool));
        for (int i = 0; i < n_bars; i++) {
            regime_masks[r][i] = (regimes[i] == (regime_t)r);
        }
    }

    fprintf(stderr, " %zu signals\n", N_SIGNALS);

    /* ── 6. Scan per-regime combos ──────────────────────────────────────── */
    fprintf(stderr, "[6/7] Scanning per-regime combos...\n");

    int max_results = 50000;
    regime_result_t *results = calloc((size_t)max_results, sizeof(regime_result_t));
    int n_results = 0;

    bool *combo_sig = calloc((size_t)n_bars, sizeof(bool));
    long eval_count = 0;

    for (int regime = 0; regime < REGIME_COUNT; regime++) {
        if (rstats[regime].count < 50) {
            fprintf(stderr, "  %s: too few bars (%d), skip\n",
                    REGIME_NAMES[regime], rstats[regime].count);
            continue;
        }

        fprintf(stderr, "  Scanning %s (%d bars)...\n",
                REGIME_NAMES[regime], rstats[regime].count);

        /* 1-2 signal combos */
        for (int a = 0; a < (int)N_SIGNALS; a++) {
            for (int b = a; b < (int)N_SIGNALS; b++) {
                int n_sigs = (b == a) ? 1 : 2;
                char combo_name[256];
                if (n_sigs == 1)
                    snprintf(combo_name, sizeof(combo_name), "%s", ALL_SIGNALS[a].name);
                else
                    snprintf(combo_name, sizeof(combo_name), "%s+%s",
                             ALL_SIGNALS[a].name, ALL_SIGNALS[b].name);

                /* Build combo mask (AND signals AND regime) */
                int combo_count = 0;
                for (int i = 0; i < n_bars; i++) {
                    bool active = sig_active[a][i];
                    if (n_sigs >= 2) active = active && sig_active[b][i];
                    active = active && regime_masks[regime][i];
                    combo_sig[i] = active;
                    if (active) combo_count++;
                }

                if (combo_count < MIN_OCCURRENCES) continue;

                /* For each direction × TP × SL */
                for (int dir = -1; dir <= 1; dir += 2) {
                    for (int ti = 0; ti < (int)N_TP; ti++) {
                        for (int si = 0; si < (int)N_SL; si++) {
                            eval_count++;

                            sim_result_t r = simulate_single_pos(
                                bars, n_bars, combo_sig,
                                dir, TP_GRID[ti], SL_GRID[si],
                                do_montecarlo);

                            if (r.total_trades < MIN_OCCURRENCES) {
                                sim_result_free(&r);
                                continue;
                            }

                            double ev = compute_ev(&r);
                            double wr = compute_wr(&r);
                            double pf = compute_pf(&r);
                            double sharpe = compute_sharpe(&r);

                            if (ev <= 0 || pf < 1.0) {
                                sim_result_free(&r);
                                continue;
                            }

                            /* Store result */
                            if (n_results < max_results) {
                                regime_result_t *e = &results[n_results];
                                strncpy(e->combo_name, combo_name, sizeof(e->combo_name) - 1);
                                strncpy(e->direction, dir == 1 ? "LONG" : "SHORT",
                                        sizeof(e->direction) - 1);
                                e->regime = regime;
                                e->tp = TP_GRID[ti];
                                e->sl = SL_GRID[si];
                                e->count = r.total_trades;
                                e->wr = wr;
                                e->ev = ev;
                                e->sharpe = sharpe;
                                e->pf = pf;
                                e->ci_lower = wilson_ci_lower(r.wins, r.total_trades);
                                e->trade_pnls = r.trade_pnls;
                                e->n_trades = r.total_trades;
                                r.trade_pnls = NULL; /* transfer ownership */
                                n_results++;
                            }

                            sim_result_free(&r);
                        }
                    }
                }
            }
        }
    }

    fprintf(stderr, "  %ld evaluations, %d results passed filters\n", eval_count, n_results);

    /* Sort by EV */
    qsort(results, (size_t)n_results, sizeof(regime_result_t), cmp_by_ev);

    /* Deduplicate: keep best TP/SL per (regime, direction, combo_name) */
    /* Results are already sorted by EV, so first occurrence is best */
    bool *keep = calloc((size_t)n_results, sizeof(bool));
    int n_unique = 0;
    for (int i = 0; i < n_results; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (!keep[j]) continue;
            if (results[i].regime == results[j].regime &&
                strcmp(results[i].direction, results[j].direction) == 0 &&
                strcmp(results[i].combo_name, results[j].combo_name) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            keep[i] = true;
            n_unique++;
        }
    }

    /* Compact */
    int w = 0;
    for (int i = 0; i < n_results; i++) {
        if (keep[i]) {
            if (w != i) {
                /* Free discarded trade_pnls before overwriting */
                free(results[w].trade_pnls);
                results[w] = results[i];
                if (w != i) results[i].trade_pnls = NULL;
            }
            w++;
        } else {
            free(results[i].trade_pnls);
            results[i].trade_pnls = NULL;
        }
    }
    n_results = n_unique;
    free(keep);

    /* Print top results */
    for (int regime = 0; regime < REGIME_COUNT; regime++) {
        for (int dir = 0; dir < 2; dir++) {
            const char *dir_str = dir == 0 ? "LONG" : "SHORT";
            int printed = 0;
            fprintf(stderr, "\n  Top %s %s:\n", REGIME_NAMES[regime], dir_str);
            fprintf(stderr, "  %-40s %8s %6s %6s %8s %6s\n",
                    "Signal", "TP/SL", "Count", "WR%", "EV%", "PF");
            fprintf(stderr, "  %-40s %8s %6s %6s %8s %6s\n",
                    "----------------------------------------", "--------",
                    "------", "------", "--------", "------");
            for (int i = 0; i < n_results && printed < 10; i++) {
                if (results[i].regime != regime) continue;
                if ((dir == 0 && strcmp(results[i].direction, "LONG") != 0) ||
                    (dir == 1 && strcmp(results[i].direction, "SHORT") != 0))
                    continue;
                fprintf(stderr, "  %-40s %3.0f/%3.0f %6d %5.1f%% %7.3f%% %5.2f\n",
                        results[i].combo_name, results[i].tp, results[i].sl,
                        results[i].count, results[i].wr, results[i].ev,
                        results[i].pf > 999 ? 999.0 : results[i].pf);
                printed++;
            }
            if (printed == 0) fprintf(stderr, "    (none)\n");
        }
    }

    /* ── Walk-forward validation (if requested) ────────────────────────── */
    if (do_validate) {
        fprintf(stderr, "\n[6b] Walk-forward validation...\n");
        for (int i = 0; i < n_results && i < 30; i++) {
            /* Rebuild the combo mask for this result */
            /* Find signal indices from combo_name */
            char name_buf[256];
            strncpy(name_buf, results[i].combo_name, sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';

            int sig_indices[3] = {-1, -1, -1};
            int n_combo_sigs = 0;
            char *tok = strtok(name_buf, "+");
            while (tok && n_combo_sigs < 3) {
                for (int s = 0; s < (int)N_SIGNALS; s++) {
                    if (strcmp(ALL_SIGNALS[s].name, tok) == 0) {
                        sig_indices[n_combo_sigs++] = s;
                        break;
                    }
                }
                tok = strtok(NULL, "+");
            }

            if (n_combo_sigs == 0) continue;

            /* Build full mask (signal AND regime) */
            for (int j = 0; j < n_bars; j++) {
                bool active = sig_active[sig_indices[0]][j];
                for (int k = 1; k < n_combo_sigs; k++) {
                    if (sig_indices[k] >= 0)
                        active = active && sig_active[sig_indices[k]][j];
                }
                active = active && regime_masks[results[i].regime][j];
                combo_sig[j] = active;
            }

            int direction = strcmp(results[i].direction, "LONG") == 0 ? 1 : -1;

            wf_result_t wf = walk_forward_validate(
                bars, n_bars, combo_sig,
                direction, results[i].tp, results[i].sl);

            results[i].wf_valid = wf.valid;
            results[i].wf_robust = wf.robust;
            results[i].wf_oos_ev = wf.oos_ev;
            results[i].wf_degradation = wf.degradation_pct;

            if (wf.valid) {
                fprintf(stderr, "    %s_%s_%s: OOS_EV=%.3f%% deg=%.0f%% -> %s\n",
                        REGIME_NAMES[results[i].regime], results[i].direction,
                        results[i].combo_name, wf.oos_ev, wf.degradation_pct,
                        wf.robust ? "ROBUST" : "FRAGILE");
            }
        }
    }

    /* ── Monte Carlo (if requested) ────────────────────────────────────── */
    mc_result_t mc_global = {0};
    if (do_montecarlo) {
        fprintf(stderr, "\n[6c] Monte Carlo simulation...\n");
        xo_seed(MC_SEED);

        /* Pool all trade PnLs from top results */
        int total_mc_trades = 0;
        for (int i = 0; i < n_results && i < 30; i++) {
            total_mc_trades += results[i].n_trades;
        }

        if (total_mc_trades >= 20) {
            double *all_pnls = malloc((size_t)total_mc_trades * sizeof(double));
            if (all_pnls) {
                int idx = 0;
                for (int i = 0; i < n_results && i < 30; i++) {
                    if (results[i].trade_pnls) {
                        memcpy(&all_pnls[idx], results[i].trade_pnls,
                               (size_t)results[i].n_trades * sizeof(double));
                        idx += results[i].n_trades;
                    }
                }

                mc_global = monte_carlo_bootstrap(all_pnls, total_mc_trades,
                                                    672.0, 5.0, MC_N_SIMS);

                if (mc_global.valid) {
                    fprintf(stderr, "  Global MC: P(ruin)=%.1f%%, P95 DD=%.1f%%, "
                            "median return=%.1f%%\n",
                            mc_global.p_ruin, mc_global.p95_drawdown,
                            mc_global.median_return);
                }

                free(all_pnls);
            }
        } else {
            fprintf(stderr, "  Not enough trades for MC (%d)\n", total_mc_trades);
        }
    }

    /* ── 7. Generate report ─────────────────────────────────────────────── */
    fprintf(stderr, "\n[7/7] Generating report...\n");
    generate_report(coin, tf, n_bars, rstats, trans_matrix,
                    results, n_results, &mc_global, date_range);

    /* ── Cleanup ────────────────────────────────────────────────────────── */
    for (int i = 0; i < n_results; i++)
        free(results[i].trade_pnls);
    free(results);
    free(combo_sig);
    for (int r = 0; r < REGIME_COUNT; r++)
        free(regime_masks[r]);
    for (int s = 0; s < (int)N_SIGNALS; s++)
        free(sig_active[s]);
    free(sig_active);
    free(regimes);
    free(snaps);
    free(bars_all);

    fprintf(stderr, "\n======================================================================\n");
    fprintf(stderr, "  Regime Analyzer complete for %s\n", coin);
    fprintf(stderr, "======================================================================\n");

    return 0;
}
