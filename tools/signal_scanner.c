/*
 * Signal Scanner — C-native signal combo scanner
 *
 * Loads 5m candles from SQLite, aggregates to 1h, computes indicators
 * via tb_indicators_compute(), scans all 1/2/3-signal combos over
 * a TP/SL grid, simulates single-position sequentially, and outputs
 * walk-forward results sorted by OOS Sharpe.
 *
 * Usage: ./signal_scanner <coin> [n_days] [direction] [max_combo]
 *   coin:      BTC, ETH, SOL, DOGE
 *   n_days:    history length (default 2000)
 *   direction: long, short, both (default both)
 *   max_combo: 1, 2, or 3 (default 2)
 *
 * Output: TSV to stdout, progress to stderr
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

/* ── Config ────────────────────────────────────────────────────────────── */
#define CACHE_DB_PATH   "./data/candle_cache.db"
#define MAX_5M_CANDLES  2000000
#define MAX_1H_CANDLES  50000
#define FEE_ROUND_TRIP  0.0006   /* 0.06% maker+taker */
#define MAX_HOLD_BARS   24       /* max 24h hold */
#define MIN_OCCURRENCES 20       /* minimum signal occurrences */
#define IS_SPLIT        0.70     /* 70% in-sample */

/* TP/SL grid */
static const double TP_GRID[] = {1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 6.0};
static const double SL_GRID[] = {0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0};
#define N_TP (sizeof(TP_GRID) / sizeof(TP_GRID[0]))
#define N_SL (sizeof(SL_GRID) / sizeof(SL_GRID[0]))

/* ── Signal definition ─────────────────────────────────────────────────── */
typedef bool (*signal_fn)(const tb_indicators_snapshot_t *snap,
                          const tb_candle_input_t *candle);

typedef struct {
    const char *name;
    signal_fn   check;
} signal_def_t;

/* ── Signal implementations ─────────────────────────────────────────────── */

/* RSI signals */
static bool sig_rsi_gt65(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->rsi_14 > 65;
}
static bool sig_rsi_gt70(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->rsi_14 > 70;
}
static bool sig_rsi_lt35(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->rsi_14 < 35;
}
static bool sig_rsi_lt30(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->rsi_14 < 30;
}
static bool sig_rsi_40_60(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->rsi_14 >= 40 && s->rsi_14 <= 60;
}

/* Volatility signals */
static bool sig_low_vol(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    return c->close > 0 && (s->atr_14 / c->close) < 0.005;
}
static bool sig_very_low_vol(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    return c->close > 0 && (s->atr_14 / c->close) < 0.003;
}
static bool sig_high_vol(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    return c->close > 0 && (s->atr_14 / c->close) > 0.01;
}
static bool sig_atr_p80(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->atr_pct_rank > 0.8;
}
static bool sig_atr_p20(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->atr_pct_rank < 0.2;
}

/* ADX signals */
static bool sig_adx_gt25(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->adx_14 > 25;
}
static bool sig_adx_lt20(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->adx_14 < 20;
}
static bool sig_adx_gt30(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->adx_14 > 30;
}

/* DI signals */
static bool sig_di_bull(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->di_bull;
}
static bool sig_di_bear(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->di_bear;
}

/* MACD signals */
static bool sig_macd_gt0(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->macd_histogram > 0;
}
static bool sig_macd_lt0(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->macd_histogram < 0;
}
static bool sig_macd_accel(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->macd_hist_incr;
}
static bool sig_macd_decel(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->macd_hist_decr;
}
static bool sig_macd_bull_cross(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->macd_bullish_cross;
}

/* Bollinger signals */
static bool sig_bb_squeeze(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->bb_squeeze;
}
static bool sig_above_bb_upper(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    return c->close > s->bb_upper && s->bb_upper > 0;
}
static bool sig_below_bb_lower(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    return c->close < s->bb_lower && s->bb_lower > 0;
}
static bool sig_below_bb_mid(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    return c->close < s->bb_middle && s->bb_middle > 0;
}

/* SMA signals */
static bool sig_above_sma200(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->above_sma_200;
}
static bool sig_below_sma200(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return !s->above_sma_200 && s->sma_200 > 0;
}
static bool sig_golden_cross(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->golden_cross;
}
static bool sig_death_cross(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return !s->golden_cross && s->sma_200 > 0 && s->sma_50 > 0;
}
static bool sig_sma20_far_above(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->sma20_dist_pct > 2.0;
}
static bool sig_sma20_far_below(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->sma20_dist_pct < -2.0;
}

/* EMA signals */
static bool sig_ema12_far_above(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->ema12_dist_pct > 2.0;
}
static bool sig_ema12_far_below(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->ema12_dist_pct < -2.0;
}
static bool sig_ema_up(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->ema_12 > s->ema_26 && s->ema_26 > 0;
}
static bool sig_ema_down(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->ema_12 < s->ema_26 && s->ema_26 > 0;
}

/* Volume signals */
static bool sig_vol_spike_2x(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->vol_ratio > 2.0;
}
static bool sig_vol_spike_3x(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->vol_ratio > 3.0;
}
static bool sig_low_volume(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->vol_ratio < 0.5;
}

/* OBV signals */
static bool sig_obv_above_sma(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->obv > s->obv_sma && s->obv_sma != 0;
}
static bool sig_obv_below_sma(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->obv < s->obv_sma && s->obv_sma != 0;
}

/* MFI signals */
static bool sig_mfi_gt70(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->mfi_14 > 70;
}
static bool sig_mfi_lt20(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->mfi_14 < 20;
}

/* CMF signals */
static bool sig_cmf_pos(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->cmf_20 > 0.05;
}
static bool sig_cmf_neg(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->cmf_20 < -0.05;
}

/* Squeeze momentum */
static bool sig_squeeze_on(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->squeeze_on;
}
static bool sig_squeeze_bull(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->squeeze_mom > 0;
}
static bool sig_squeeze_bear(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->squeeze_mom < 0;
}

/* Range signals */
static bool sig_range_wide(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->range_pct_rank > 0.8;
}
static bool sig_range_narrow(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->range_pct_rank < 0.2;
}

/* Candle patterns */
static bool sig_bullish_engulf(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->bullish_engulf;
}
static bool sig_bearish_engulf(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->bearish_engulf;
}
static bool sig_hammer(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->hammer;
}
static bool sig_shooting_star(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->shooting_star;
}
static bool sig_doji(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->doji;
}

/* Consecutive candle signals */
static bool sig_consec_green_3(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->consec_green >= 3;
}
static bool sig_consec_red_3(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->consec_red >= 3;
}

/* Supertrend */
static bool sig_supertrend_bull(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->supertrend_up;
}
static bool sig_supertrend_bear(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return !s->supertrend_up && s->supertrend > 0;
}

/* PSAR */
static bool sig_psar_bull(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->psar_up;
}
static bool sig_psar_bear(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return !s->psar_up && s->psar > 0;
}

/* Ichimoku */
static bool sig_ichi_bullish(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->ichi_bullish;
}

/* RSI divergence */
static bool sig_rsi_bull_div(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->rsi_bull_div;
}
static bool sig_rsi_bear_div(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->rsi_bear_div;
}

/* ROC */
static bool sig_roc_positive(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->roc_12 > 0;
}
static bool sig_roc_negative(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->roc_12 < 0;
}
static bool sig_roc_strong(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->roc_12 > 3.0 || s->roc_12 < -3.0;
}

/* Z-Score */
static bool sig_zscore_high(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->zscore_20 > 2.0;
}
static bool sig_zscore_low(const tb_indicators_snapshot_t *s, const tb_candle_input_t *c) {
    (void)c; return s->zscore_20 < -2.0;
}

/* ── All signals array ──────────────────────────────────────────────────── */
static const signal_def_t ALL_SIGNALS[] = {
    /* RSI */
    {"rsi_gt65",         sig_rsi_gt65},
    {"rsi_gt70",         sig_rsi_gt70},
    {"rsi_lt35",         sig_rsi_lt35},
    {"rsi_lt30",         sig_rsi_lt30},
    {"rsi_40_60",        sig_rsi_40_60},
    /* Volatility */
    {"low_vol",          sig_low_vol},
    {"very_low_vol",     sig_very_low_vol},
    {"high_vol",         sig_high_vol},
    {"atr_p80",          sig_atr_p80},
    {"atr_p20",          sig_atr_p20},
    /* ADX */
    {"adx_gt25",         sig_adx_gt25},
    {"adx_lt20",         sig_adx_lt20},
    {"adx_gt30",         sig_adx_gt30},
    /* DI */
    {"di_bull",          sig_di_bull},
    {"di_bear",          sig_di_bear},
    /* MACD */
    {"macd_gt0",         sig_macd_gt0},
    {"macd_lt0",         sig_macd_lt0},
    {"macd_accel",       sig_macd_accel},
    {"macd_decel",       sig_macd_decel},
    {"macd_bull_cross",  sig_macd_bull_cross},
    /* BB */
    {"bb_squeeze",       sig_bb_squeeze},
    {"above_bb_upper",   sig_above_bb_upper},
    {"below_bb_lower",   sig_below_bb_lower},
    {"below_bb_mid",     sig_below_bb_mid},
    /* SMA */
    {"above_sma200",     sig_above_sma200},
    {"below_sma200",     sig_below_sma200},
    {"golden_cross",     sig_golden_cross},
    {"death_cross",      sig_death_cross},
    {"sma20_far_above",  sig_sma20_far_above},
    {"sma20_far_below",  sig_sma20_far_below},
    /* EMA */
    {"ema12_far_above",  sig_ema12_far_above},
    {"ema12_far_below",  sig_ema12_far_below},
    {"ema_up",           sig_ema_up},
    {"ema_down",         sig_ema_down},
    /* Volume */
    {"vol_spike_2x",     sig_vol_spike_2x},
    {"vol_spike_3x",     sig_vol_spike_3x},
    {"low_volume",       sig_low_volume},
    /* OBV */
    {"obv_above_sma",    sig_obv_above_sma},
    {"obv_below_sma",    sig_obv_below_sma},
    /* MFI */
    {"mfi_gt70",         sig_mfi_gt70},
    {"mfi_lt20",         sig_mfi_lt20},
    /* CMF */
    {"cmf_pos",          sig_cmf_pos},
    {"cmf_neg",          sig_cmf_neg},
    /* Squeeze */
    {"squeeze_on",       sig_squeeze_on},
    {"squeeze_bull",     sig_squeeze_bull},
    {"squeeze_bear",     sig_squeeze_bear},
    /* Range */
    {"range_wide",       sig_range_wide},
    {"range_narrow",     sig_range_narrow},
    /* Candle patterns */
    {"bullish_engulf",   sig_bullish_engulf},
    {"bearish_engulf",   sig_bearish_engulf},
    {"hammer",           sig_hammer},
    {"shooting_star",    sig_shooting_star},
    {"doji",             sig_doji},
    /* Consecutive */
    {"consec_green_3",   sig_consec_green_3},
    {"consec_red_3",     sig_consec_red_3},
    /* Supertrend */
    {"supertrend_bull",  sig_supertrend_bull},
    {"supertrend_bear",  sig_supertrend_bear},
    /* PSAR */
    {"psar_bull",        sig_psar_bull},
    {"psar_bear",        sig_psar_bear},
    /* Ichimoku */
    {"ichi_bullish",     sig_ichi_bullish},
    /* Divergence */
    {"rsi_bull_div",     sig_rsi_bull_div},
    {"rsi_bear_div",     sig_rsi_bear_div},
    /* ROC */
    {"roc_positive",     sig_roc_positive},
    {"roc_negative",     sig_roc_negative},
    {"roc_strong",       sig_roc_strong},
    /* Z-Score */
    {"zscore_high",      sig_zscore_high},
    {"zscore_low",       sig_zscore_low},
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
    int rc = sqlite3_prepare_v2(db,
        "SELECT time_ms, open, high, low, close, volume FROM candles "
        "WHERE coin=? AND interval='5m' AND time_ms>=? AND time_ms<=? "
        "ORDER BY time_ms ASC",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
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

/* ── Aggregate 5m → 1h ──────────────────────────────────────────────────── */
static int aggregate_to_1h(const tb_candle_input_t *m5, int n5,
                            tb_candle_input_t *out, int max) {
    if (n5 < 12) return 0;

    int n1h = 0;
    int i = 0;

    /* Align to 1h boundary (time_ms % 3600000 == 0) */
    while (i < n5 && (m5[i].time_ms % 3600000LL) != 0)
        i++;

    while (i + 11 < n5 && n1h < max) {
        int64_t hour_start = m5[i].time_ms;
        /* Verify all 12 candles belong to same hour */
        if (m5[i + 11].time_ms != hour_start + 11 * 300000LL) {
            /* Gap detected — skip to next aligned hour */
            i++;
            while (i < n5 && (m5[i].time_ms % 3600000LL) != 0)
                i++;
            continue;
        }

        out[n1h].time_ms = hour_start;
        out[n1h].open    = m5[i].open;
        out[n1h].close   = m5[i + 11].close;
        out[n1h].high    = m5[i].high;
        out[n1h].low     = m5[i].low;
        out[n1h].volume  = 0;

        for (int j = 0; j < 12; j++) {
            if (m5[i + j].high > out[n1h].high) out[n1h].high = m5[i + j].high;
            if (m5[i + j].low  < out[n1h].low)  out[n1h].low  = m5[i + j].low;
            out[n1h].volume += m5[i + j].volume;
        }

        n1h++;
        i += 12;
    }

    return n1h;
}

/* ── Single-position trade simulation ───────────────────────────────────── */
typedef struct {
    int    total_trades;
    int    wins;
    double total_pnl;          /* sum of per-trade PnL% */
    double sum_sq_ret;         /* sum of squared returns (for Sharpe) */
    double sum_neg_sq_ret;     /* sum of squared negative returns (for Sortino) */
    double max_drawdown;       /* maximum drawdown % */
} sim_result_t;

static sim_result_t simulate_single_pos(const tb_candle_input_t *candles, int n,
                                          const bool *signal_active,
                                          int direction, /* 1=long, -1=short */
                                          double tp_pct, double sl_pct) {
    sim_result_t r = {0};
    double equity = 100.0;
    double peak = equity;

    int i = 0;
    while (i < n) {
        if (!signal_active[i]) { i++; continue; }

        /* Entry at close of signal bar */
        double entry = candles[i].close;
        if (entry <= 0) { i++; continue; }

        double tp_price, sl_price;
        if (direction == 1) { /* long */
            tp_price = entry * (1.0 + tp_pct / 100.0);
            sl_price = entry * (1.0 - sl_pct / 100.0);
        } else { /* short */
            tp_price = entry * (1.0 - tp_pct / 100.0);
            sl_price = entry * (1.0 + sl_pct / 100.0);
        }

        /* Simulate forward */
        double exit_price = 0;
        int bars_held = 0;

        for (int j = i + 1; j < n && bars_held < MAX_HOLD_BARS; j++) {
            bars_held++;

            if (direction == 1) { /* long */
                /* Check SL first (conservative) */
                if (candles[j].low <= sl_price) {
                    exit_price = sl_price;
                    break;
                }
                if (candles[j].high >= tp_price) {
                    exit_price = tp_price;
                    break;
                }
            } else { /* short */
                if (candles[j].high >= sl_price) {
                    exit_price = sl_price;
                    break;
                }
                if (candles[j].low <= tp_price) {
                    exit_price = tp_price;
                    break;
                }
            }

            if (bars_held >= MAX_HOLD_BARS) {
                exit_price = candles[j].close;
                break;
            }
        }

        if (exit_price <= 0) {
            /* Ran out of data */
            i++;
            continue;
        }

        /* Calculate PnL */
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

        /* Skip ahead (no overlapping positions) */
        i += bars_held + 1;
    }

    return r;
}

/* ── Compute Sharpe/Sortino from sim_result ─────────────────────────────── */
static double compute_sharpe(const sim_result_t *r) {
    if (r->total_trades < 2) return 0;
    double mean = (r->total_pnl / 100.0) / r->total_trades;
    double var = r->sum_sq_ret / r->total_trades - mean * mean;
    double std = sqrt(var > 0 ? var : 0);
    if (std < 1e-6) return mean > 0 ? 99.0 : 0;  /* cap when near-zero variance */
    /* Annualize: ~4 trades/week × 52 weeks */
    double s = (mean / std) * sqrt(208.0);
    return s > 99.0 ? 99.0 : s;
}

static double compute_sortino(const sim_result_t *r) {
    if (r->total_trades < 2) return 0;
    double mean = (r->total_pnl / 100.0) / r->total_trades;
    double downside_std = sqrt(r->sum_neg_sq_ret / r->total_trades);
    if (downside_std < 1e-6) return mean > 0 ? 99.0 : 0;
    double s = (mean / downside_std) * sqrt(208.0);
    return s > 99.0 ? 99.0 : s;
}

/* ── Result entry (for sorting) ─────────────────────────────────────────── */
typedef struct {
    char   combo_name[256];
    char   direction[8];
    double tp, sl;
    /* IS metrics */
    int    is_trades;
    double is_wr;
    double is_ev;
    double is_sharpe;
    double is_sortino;
    double is_dd;
    /* OOS metrics */
    int    oos_trades;
    double oos_wr;
    double oos_ev;
    double oos_sharpe;
    double oos_sortino;
    double oos_dd;
} result_entry_t;

static int cmp_by_oos_sharpe(const void *a, const void *b) {
    double sa = ((const result_entry_t *)a)->oos_sharpe;
    double sb = ((const result_entry_t *)b)->oos_sharpe;
    if (sb > sa) return 1;
    if (sb < sa) return -1;
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    tb_log_init("./logs", TB_LOG_LVL_WARN);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <coin> [n_days] [direction] [max_combo]\n", argv[0]);
        fprintf(stderr, "  direction: long, short, both (default: both)\n");
        fprintf(stderr, "  max_combo: 1, 2, 3 (default: 2)\n");
        return 1;
    }

    const char *coin = argv[1];
    int n_days       = argc >= 3 ? atoi(argv[2]) : 2000;
    const char *dir_str = argc >= 4 ? argv[3] : "both";
    int max_combo    = argc >= 5 ? atoi(argv[4]) : 2;

    if (n_days < 30) n_days = 30;
    if (n_days > 5000) n_days = 5000;
    if (max_combo < 1) max_combo = 1;
    if (max_combo > 3) max_combo = 3;

    int scan_long  = (strcmp(dir_str, "both") == 0 || strcmp(dir_str, "long") == 0);
    int scan_short = (strcmp(dir_str, "both") == 0 || strcmp(dir_str, "short") == 0);

    /* ── Load 5m candles ────────────────────────────────────────────── */
    fprintf(stderr, "Loading 5m candles for %s (%d days)...\n", coin, n_days);

    tb_candle_input_t *m5 = calloc(MAX_5M_CANDLES, sizeof(tb_candle_input_t));
    if (!m5) { fprintf(stderr, "malloc failed\n"); return 1; }

    int n5 = load_5m_candles(coin, n_days, m5, MAX_5M_CANDLES);
    if (n5 < 100) {
        fprintf(stderr, "ERROR: only %d 5m candles — run candle_fetcher first\n", n5);
        free(m5);
        return 1;
    }
    fprintf(stderr, "Loaded %d 5m candles\n", n5);

    /* ── Aggregate to 1h ─────────────────────────────────────────────── */
    tb_candle_input_t *h1 = calloc(MAX_1H_CANDLES, sizeof(tb_candle_input_t));
    if (!h1) { free(m5); return 1; }

    int n1h = aggregate_to_1h(m5, n5, h1, MAX_1H_CANDLES);
    free(m5);  /* no longer needed */

    if (n1h < 250) {
        fprintf(stderr, "ERROR: only %d 1h candles after aggregation\n", n1h);
        free(h1);
        return 1;
    }
    fprintf(stderr, "Aggregated to %d 1h candles (%.1f years)\n",
            n1h, n1h / 8760.0);

    /* ── Compute indicators for each bar ─────────────────────────────── */
    fprintf(stderr, "Computing indicators...\n");

    /* We need a rolling window — compute snapshot for each bar using
       the last 250 candles (sufficient for SMA200 + warmup) */
    int window = 250;
    int n_bars = n1h - window;  /* bars with valid indicators */
    if (n_bars < 100) {
        fprintf(stderr, "ERROR: not enough bars for indicator warmup\n");
        free(h1);
        return 1;
    }

    tb_indicators_snapshot_t *snaps = calloc(n_bars, sizeof(tb_indicators_snapshot_t));
    if (!snaps) { free(h1); return 1; }

    for (int i = 0; i < n_bars; i++) {
        snaps[i] = tb_indicators_compute(&h1[i], window + 1);
    }

    /* Shift candles pointer so bar[0] aligns with snaps[0] */
    tb_candle_input_t *bars = &h1[window];

    fprintf(stderr, "Computed indicators for %d bars\n", n_bars);

    /* ── Pre-compute signal bitmask per bar ──────────────────────────── */
    /* Use bool arrays for each signal */
    bool **sig_active = calloc(N_SIGNALS, sizeof(bool *));
    for (int s = 0; s < (int)N_SIGNALS; s++) {
        sig_active[s] = calloc(n_bars, sizeof(bool));
        for (int i = 0; i < n_bars; i++) {
            if (snaps[i].valid)
                sig_active[s][i] = ALL_SIGNALS[s].check(&snaps[i], &bars[i]);
        }
    }

    /* ── IS/OOS split ────────────────────────────────────────────────── */
    int is_end = (int)(n_bars * IS_SPLIT);
    int oos_start = is_end;
    fprintf(stderr, "IS: bars 0-%d (%d), OOS: bars %d-%d (%d)\n",
            is_end - 1, is_end, oos_start, n_bars - 1, n_bars - oos_start);

    /* ── Prepare combo signals + simulate ────────────────────────────── */
    /* Count combos */
    long total_combos = 0;
    if (max_combo >= 1) total_combos += N_SIGNALS;
    if (max_combo >= 2) total_combos += N_SIGNALS * (N_SIGNALS - 1) / 2;
    if (max_combo >= 3) total_combos += N_SIGNALS * (N_SIGNALS - 1) * (N_SIGNALS - 2) / 6;

    int n_dirs = scan_long + scan_short;
    long total_evals = total_combos * n_dirs * N_TP * N_SL;

    fprintf(stderr, "Scanning %ld combos × %d dirs × %zu TP × %zu SL = %ld evaluations\n",
            total_combos, n_dirs, N_TP, N_SL, total_evals);

    /* Allocate results (upper bound) */
    int max_results = 100000;
    result_entry_t *results = calloc(max_results, sizeof(result_entry_t));
    int n_results = 0;

    bool *combo_is  = calloc(is_end, sizeof(bool));
    bool *combo_oos = calloc(n_bars - oos_start, sizeof(bool));

    long eval_count = 0;
    long last_progress = 0;

    /* Iterate combos */
    for (int a = 0; a < (int)N_SIGNALS; a++) {
        int combo_end = (max_combo >= 2) ? (int)N_SIGNALS : a + 1;

        for (int b = a; b < combo_end; b++) {
            int combo_end_c = (max_combo >= 3 && b > a) ? (int)N_SIGNALS : b + 1;
            if (b == a) combo_end_c = b + 1;  /* single signal: don't iterate c */

            for (int c = b; c < combo_end_c; c++) {
                if (c == b && b > a) { /* 2-signal combo, c not used */ }
                if (c > b && b == a) continue; /* skip: a==b but c>b makes no sense */

                /* Determine combo name and size */
                char combo_name[256];
                int n_sigs;
                if (b == a && c == b) {
                    /* Single signal */
                    snprintf(combo_name, sizeof(combo_name), "%s", ALL_SIGNALS[a].name);
                    n_sigs = 1;
                } else if (c == b) {
                    /* 2-signal combo */
                    snprintf(combo_name, sizeof(combo_name), "%s+%s",
                             ALL_SIGNALS[a].name, ALL_SIGNALS[b].name);
                    n_sigs = 2;
                } else {
                    /* 3-signal combo */
                    snprintf(combo_name, sizeof(combo_name), "%s+%s+%s",
                             ALL_SIGNALS[a].name, ALL_SIGNALS[b].name, ALL_SIGNALS[c].name);
                    n_sigs = 3;
                }

                /* Build combo active arrays (AND of signals) */
                int is_occ = 0, oos_occ = 0;

                for (int i = 0; i < is_end; i++) {
                    bool active = sig_active[a][i];
                    if (n_sigs >= 2) active = active && sig_active[b][i];
                    if (n_sigs >= 3) active = active && sig_active[c][i];
                    combo_is[i] = active;
                    if (active) is_occ++;
                }

                for (int i = 0; i < n_bars - oos_start; i++) {
                    bool active = sig_active[a][oos_start + i];
                    if (n_sigs >= 2) active = active && sig_active[b][oos_start + i];
                    if (n_sigs >= 3) active = active && sig_active[c][oos_start + i];
                    combo_oos[i] = active;
                    if (active) oos_occ++;
                }

                /* Skip if too few occurrences */
                if (is_occ < MIN_OCCURRENCES || oos_occ < MIN_OCCURRENCES / 2) {
                    eval_count += n_dirs * N_TP * N_SL;
                    continue;
                }

                /* For each direction × TP × SL */
                for (int dir = -1; dir <= 1; dir += 2) {
                    if (dir == 1 && !scan_long) continue;
                    if (dir == -1 && !scan_short) continue;

                    for (int ti = 0; ti < (int)N_TP; ti++) {
                        for (int si = 0; si < (int)N_SL; si++) {
                            eval_count++;

                            /* Progress */
                            if (eval_count - last_progress > 50000) {
                                fprintf(stderr, "\r  [%ld/%ld] %.1f%% — %d results",
                                        eval_count, total_evals,
                                        100.0 * eval_count / total_evals, n_results);
                                last_progress = eval_count;
                            }

                            /* Simulate IS */
                            sim_result_t r_is = simulate_single_pos(
                                bars, is_end, combo_is,
                                dir, TP_GRID[ti], SL_GRID[si]);

                            if (r_is.total_trades < MIN_OCCURRENCES) continue;
                            double is_sharpe = compute_sharpe(&r_is);
                            if (is_sharpe < 0.5) continue;  /* pre-filter */

                            /* Simulate OOS */
                            sim_result_t r_oos = simulate_single_pos(
                                &bars[oos_start], n_bars - oos_start, combo_oos,
                                dir, TP_GRID[ti], SL_GRID[si]);

                            if (r_oos.total_trades < MIN_OCCURRENCES / 2) continue;

                            double oos_sharpe = compute_sharpe(&r_oos);
                            if (oos_sharpe < 0.3) continue;  /* filter noise */

                            /* Store result */
                            if (n_results < max_results) {
                                result_entry_t *e = &results[n_results];
                                strncpy(e->combo_name, combo_name, sizeof(e->combo_name) - 1);
                                strncpy(e->direction, dir == 1 ? "LONG" : "SHORT",
                                        sizeof(e->direction) - 1);
                                e->tp = TP_GRID[ti];
                                e->sl = SL_GRID[si];
                                e->is_trades  = r_is.total_trades;
                                e->is_wr      = r_is.total_trades > 0 ?
                                    (double)r_is.wins / r_is.total_trades * 100 : 0;
                                e->is_ev      = r_is.total_trades > 0 ?
                                    r_is.total_pnl / r_is.total_trades : 0;
                                e->is_sharpe  = is_sharpe;
                                e->is_sortino = compute_sortino(&r_is);
                                e->is_dd      = r_is.max_drawdown;
                                e->oos_trades = r_oos.total_trades;
                                e->oos_wr     = r_oos.total_trades > 0 ?
                                    (double)r_oos.wins / r_oos.total_trades * 100 : 0;
                                e->oos_ev     = r_oos.total_trades > 0 ?
                                    r_oos.total_pnl / r_oos.total_trades : 0;
                                e->oos_sharpe = oos_sharpe;
                                e->oos_sortino = compute_sortino(&r_oos);
                                e->oos_dd     = r_oos.max_drawdown;
                                n_results++;
                            }
                        }
                    }
                }
            }
        }
    }

    fprintf(stderr, "\r  Done: %ld evaluations, %d results passed filters\n",
            eval_count, n_results);

    /* ── Sort by OOS Sharpe ──────────────────────────────────────────── */
    qsort(results, n_results, sizeof(result_entry_t), cmp_by_oos_sharpe);

    /* ── Output header ───────────────────────────────────────────────── */
    printf("combo\tdir\tTP\tSL\t"
           "IS_n\tIS_WR\tIS_EV\tIS_Sharpe\tIS_Sortino\tIS_DD\t"
           "OOS_n\tOOS_WR\tOOS_EV\tOOS_Sharpe\tOOS_Sortino\tOOS_DD\n");

    int print_limit = n_results < 500 ? n_results : 500;
    for (int i = 0; i < print_limit; i++) {
        result_entry_t *e = &results[i];
        printf("%s\t%s\t%.1f\t%.1f\t"
               "%d\t%.1f\t%.3f\t%.2f\t%.2f\t%.1f\t"
               "%d\t%.1f\t%.3f\t%.2f\t%.2f\t%.1f\n",
               e->combo_name, e->direction, e->tp, e->sl,
               e->is_trades, e->is_wr, e->is_ev, e->is_sharpe, e->is_sortino, e->is_dd,
               e->oos_trades, e->oos_wr, e->oos_ev, e->oos_sharpe, e->oos_sortino, e->oos_dd);
    }

    /* ── Cleanup ─────────────────────────────────────────────────────── */
    for (int s = 0; s < (int)N_SIGNALS; s++)
        free(sig_active[s]);
    free(sig_active);
    free(combo_is);
    free(combo_oos);
    free(snaps);
    free(h1);
    free(results);

    fprintf(stderr, "Signal scanner complete: %d results for %s\n", n_results, coin);
    return 0;
}
