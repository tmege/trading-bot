"""Définition et scan des signaux d'entrée."""

import itertools
import numpy as np
import pandas as pd


# Chaque signal est une fonction qui retourne un boolean Series
SIGNAL_DEFS = {
    # RSI oversold
    "rsi_os_30": lambda df: df["rsi"] < 30,
    "rsi_os_35": lambda df: df["rsi"] < 35,
    "rsi_os_40": lambda df: df["rsi"] < 40,
    "rsi_os_45": lambda df: df["rsi"] < 45,
    # RSI overbought
    "rsi_ob_55": lambda df: df["rsi"] > 55,
    "rsi_ob_60": lambda df: df["rsi"] > 60,
    "rsi_ob_65": lambda df: df["rsi"] > 65,
    "rsi_ob_70": lambda df: df["rsi"] > 70,
    # StochRSI
    "stoch_os_20": lambda df: df["stoch_rsi_k"] < 20,
    "stoch_os_30": lambda df: df["stoch_rsi_k"] < 30,
    "stoch_ob_70": lambda df: df["stoch_rsi_k"] > 70,
    "stoch_ob_80": lambda df: df["stoch_rsi_k"] > 80,
    # Bollinger Bands
    "bb_lower": lambda df: df["close"] < df["bb_lower"] * 1.005,
    "bb_upper": lambda df: df["close"] > df["bb_upper"] * 0.995,
    "below_bb_mid": lambda df: df["close"] < df["bb_middle"],
    "above_bb_mid": lambda df: df["close"] > df["bb_middle"],
    # EMA trend
    "ema_uptrend": lambda df: df["ema_12"] > df["ema_26"],
    "ema_downtrend": lambda df: df["ema_12"] < df["ema_26"],
    # ADX
    "adx_trending": lambda df: df["adx"] > 25,
    "adx_strong": lambda df: df["adx"] > 35,
    "adx_ranging": lambda df: df["adx"] < 20,
    # MACD
    "macd_bull": lambda df: df["macd_hist"] > 0,
    "macd_bear": lambda df: df["macd_hist"] < 0,
    "macd_cross_up": lambda df: (df["macd_hist"] > 0) & (df["macd_hist"].shift(1) <= 0),
    "macd_cross_down": lambda df: (df["macd_hist"] < 0) & (df["macd_hist"].shift(1) >= 0),
    # Volatilité
    "high_vol": lambda df: df["atr_pct"] > 0.01,
    "low_vol": lambda df: df["atr_pct"] < 0.005,
    # CCI
    "cci_os": lambda df: df["cci"] < -100,
    "cci_ob": lambda df: df["cci"] > 100,
    # Williams %R
    "willr_os": lambda df: df["williams_r"] < -80,
    "willr_ob": lambda df: df["williams_r"] > -20,
    # OBV
    "obv_above_sma": lambda df: df["obv"] > df["obv_sma"],
    "obv_below_sma": lambda df: df["obv"] < df["obv_sma"],
    # BB Squeeze
    "bb_squeeze": lambda df: df["bb_squeeze"] == 1 if "bb_squeeze" in df.columns else pd.Series(False, index=df.index),
    # SMA trend
    "sma20_above_sma50": lambda df: df["sma_20"] > df["sma_50"],
    "sma20_below_sma50": lambda df: df["sma_20"] < df["sma_50"],

    # ═══ NOUVEAUX SIGNAUX AVANCÉS ═══

    # Volume
    "vol_spike_2x": lambda df: df["vol_ratio"] > 2.0,
    "vol_spike_3x": lambda df: df["vol_ratio"] > 3.0,
    "vol_dry": lambda df: df["vol_ratio"] < 0.5,

    # ATR percentile (adaptatif — mieux que low_vol/high_vol)
    "atr_p10": lambda df: df["atr_pct_rank"] < 0.10,    # très basse vol (top 10%)
    "atr_p20": lambda df: df["atr_pct_rank"] < 0.20,    # basse vol
    "atr_p80": lambda df: df["atr_pct_rank"] > 0.80,    # haute vol

    # EMA distance (mean reversion)
    "ema12_far_above": lambda df: df["ema12_dist_pct"] > 2.0,   # prix +2% au-dessus EMA12
    "ema12_far_below": lambda df: df["ema12_dist_pct"] < -2.0,  # prix -2% en-dessous EMA12
    "sma20_far_above": lambda df: df["sma20_dist_pct"] > 3.0,
    "sma20_far_below": lambda df: df["sma20_dist_pct"] < -3.0,

    # Candle patterns
    "bullish_engulf": lambda df: df["bullish_engulf"] == 1,
    "bearish_engulf": lambda df: df["bearish_engulf"] == 1,
    "hammer": lambda df: df["hammer"] == 1,
    "shooting_star": lambda df: df["shooting_star"] == 1,
    "doji": lambda df: df["doji"] == 1,

    # Consecutive candles
    "consec_green_3": lambda df: df["consec_green"] >= 3,
    "consec_red_3": lambda df: df["consec_red"] >= 3,
    "consec_green_5": lambda df: df["consec_green"] >= 5,
    "consec_red_5": lambda df: df["consec_red"] >= 5,

    # MACD momentum
    "macd_accelerating": lambda df: df["macd_hist_increasing"] == 1,
    "macd_decelerating": lambda df: df["macd_hist_decreasing"] == 1,

    # RSI divergence
    "rsi_bull_divergence": lambda df: df["rsi_bull_div"] == 1,
    "rsi_bear_divergence": lambda df: df["rsi_bear_div"] == 1,

    # Range compression (breakout imminent)
    "range_compressed": lambda df: df["range_pct_rank"] < 0.15,
    "range_wide": lambda df: df["range_pct_rank"] > 0.85,

    # DI crossover
    "di_bull": lambda df: df["di_bull"] == 1,
    "di_bear": lambda df: df["di_bear"] == 1,

    # ═══ SIGNAUX RÉGIME ═══

    # Régime filters (nécessitent classify_regime() pré-calculé dans 'regime' column)
    "regime_bull": lambda df: df["regime"] == "bull" if "regime" in df.columns else pd.Series(False, index=df.index),
    "regime_bear": lambda df: df["regime"] == "bear" if "regime" in df.columns else pd.Series(False, index=df.index),
    "regime_neutral": lambda df: df["regime"] == "neutral" if "regime" in df.columns else pd.Series(False, index=df.index),

    # Transitions de régime (premier bar du nouveau régime)
    "regime_bull_start": lambda df: (
        (df["regime"] == "bull") & (df["regime"].shift(1) != "bull")
    ) if "regime" in df.columns else pd.Series(False, index=df.index),
    "regime_bear_start": lambda df: (
        (df["regime"] == "bear") & (df["regime"].shift(1) != "bear")
    ) if "regime" in df.columns else pd.Series(False, index=df.index),

    # Volatilité très basse (ATR < 0.3% du prix — SOL best edge)
    "very_low_vol": lambda df: df["atr_pct"] < 0.003,

    # SMA200 trend
    "above_sma200": lambda df: df["close"] > df["sma_200"] if "sma_200" in df.columns else pd.Series(False, index=df.index),
    "below_sma200": lambda df: df["close"] < df["sma_200"] if "sma_200" in df.columns else pd.Series(False, index=df.index),

    # Donchian breakout (20-period)
    "dc_breakout_high": lambda df: df["close"] >= df["close"].rolling(20).max(),
    "dc_breakout_low": lambda df: df["close"] <= df["close"].rolling(20).min(),

    # ═══ CMF / MFI / SQUEEZE ═══

    # CMF (Chaikin Money Flow)
    "cmf_positive": lambda df: df["cmf"] > 0,
    "cmf_negative": lambda df: df["cmf"] < 0,
    "cmf_strong_bull": lambda df: df["cmf"] > 0.1,
    "cmf_strong_bear": lambda df: df["cmf"] < -0.1,

    # MFI (Money Flow Index)
    "mfi_os_20": lambda df: df["mfi"] < 20,
    "mfi_os_30": lambda df: df["mfi"] < 30,
    "mfi_ob_70": lambda df: df["mfi"] > 70,
    "mfi_ob_80": lambda df: df["mfi"] > 80,

    # Squeeze Momentum
    "squeeze_on": lambda df: df["squeeze_on"] == 1 if "squeeze_on" in df.columns else pd.Series(False, index=df.index),
    "squeeze_off": lambda df: df["squeeze_on"] == 0 if "squeeze_on" in df.columns else pd.Series(False, index=df.index),
    "squeeze_mom_bull": lambda df: df["squeeze_mom"] > 0 if "squeeze_mom" in df.columns else pd.Series(False, index=df.index),
    "squeeze_mom_bear": lambda df: df["squeeze_mom"] < 0 if "squeeze_mom" in df.columns else pd.Series(False, index=df.index),

    # ═══ SUPERTREND / Z-SCORE / FVG / ROC / PSAR ═══

    # Supertrend
    "supertrend_bull": lambda df: df["supertrend_up"] == 1 if "supertrend_up" in df.columns else pd.Series(False, index=df.index),
    "supertrend_bear": lambda df: df["supertrend_up"] == 0 if "supertrend_up" in df.columns else pd.Series(False, index=df.index),

    # Z-Score
    "zscore_high_2": lambda df: df["zscore"] > 2.0 if "zscore" in df.columns else pd.Series(False, index=df.index),
    "zscore_high_1": lambda df: df["zscore"] > 1.0 if "zscore" in df.columns else pd.Series(False, index=df.index),
    "zscore_low_1": lambda df: df["zscore"] < -1.0 if "zscore" in df.columns else pd.Series(False, index=df.index),
    "zscore_low_2": lambda df: df["zscore"] < -2.0 if "zscore" in df.columns else pd.Series(False, index=df.index),
    "zscore_neutral": lambda df: df["zscore"].abs() < 0.5 if "zscore" in df.columns else pd.Series(False, index=df.index),

    # FVG
    "fvg_bull": lambda df: df["fvg_bull"] == 1 if "fvg_bull" in df.columns else pd.Series(False, index=df.index),
    "fvg_bear": lambda df: df["fvg_bear"] == 1 if "fvg_bear" in df.columns else pd.Series(False, index=df.index),

    # ROC
    "roc_positive": lambda df: df["roc"] > 0 if "roc" in df.columns else pd.Series(False, index=df.index),
    "roc_negative": lambda df: df["roc"] < 0 if "roc" in df.columns else pd.Series(False, index=df.index),
    "roc_strong_up": lambda df: df["roc"] > 5.0 if "roc" in df.columns else pd.Series(False, index=df.index),
    "roc_strong_down": lambda df: df["roc"] < -5.0 if "roc" in df.columns else pd.Series(False, index=df.index),

    # Parabolic SAR
    "psar_bull": lambda df: df["psar_up"] == 1 if "psar_up" in df.columns else pd.Series(False, index=df.index),
    "psar_bear": lambda df: df["psar_up"] == 0 if "psar_up" in df.columns else pd.Series(False, index=df.index),
}


def compute_signals(df: pd.DataFrame, signal_names: list = None) -> pd.DataFrame:
    """Calcule les signaux booléens pour chaque ligne.

    Args:
        df: DataFrame avec indicateurs calculés.
        signal_names: Liste de noms de signaux. None = tous.

    Returns:
        DataFrame avec une colonne bool par signal.
    """
    if signal_names is None:
        signal_names = list(SIGNAL_DEFS.keys())

    signals_df = pd.DataFrame(index=df.index)
    for name in signal_names:
        if name not in SIGNAL_DEFS:
            print(f"  Warning: signal '{name}' inconnu, ignoré")
            continue
        try:
            signals_df[name] = SIGNAL_DEFS[name](df).fillna(False).astype(bool)
        except KeyError:
            # Colonne indicateur manquante
            signals_df[name] = False

    return signals_df


def scan_combinations(signals_df: pd.DataFrame, max_combo: int = 3,
                      min_occurrences: int = 30) -> list:
    """Scanne les combinaisons de 1, 2, et 3 signaux.

    Returns:
        Liste de tuples (combo_name, boolean_mask, count).
        Filtrée par min_occurrences.
    """
    signal_cols = list(signals_df.columns)
    results = []

    # Pré-calculer les counts des signaux individuels
    single_counts = {}
    for name in signal_cols:
        single_counts[name] = int(signals_df[name].values.sum())

    # Singles
    for name in signal_cols:
        mask = signals_df[name].values
        count = single_counts[name]
        if count >= min_occurrences:
            results.append((name, mask, count))

    # Combos de 2 (filtrer les redondants: combo count == sous-signal count)
    if max_combo >= 2:
        for combo in itertools.combinations(signal_cols, 2):
            mask = signals_df[combo[0]].values & signals_df[combo[1]].values
            count = int(mask.sum())
            if count < min_occurrences:
                continue
            # Redondant si count == count d'un des sous-signaux
            if count == single_counts.get(combo[0], 0) or count == single_counts.get(combo[1], 0):
                continue
            name = "+".join(combo)
            results.append((name, mask, count))

    # Combos de 3
    if max_combo >= 3:
        combo2_counts = {name: count for name, _, count in results if "+" in name}
        for combo in itertools.combinations(signal_cols, 3):
            mask = signals_df[combo[0]].values & signals_df[combo[1]].values & signals_df[combo[2]].values
            count = int(mask.sum())
            if count < min_occurrences:
                continue
            # Redondant si count == count d'un des sous-combos de 2
            redundant = False
            for sub in itertools.combinations(combo, 2):
                sub_name = "+".join(sub)
                if count == combo2_counts.get(sub_name, -1):
                    redundant = True
                    break
            if redundant:
                continue
            name = "+".join(combo)
            results.append((name, mask, count))

    return results


def parse_signal_spec(spec: str) -> list:
    """Parse une spec de signal comme 'ema_uptrend+rsi_os_40+stoch_os_30'.

    Returns liste de noms de signaux.
    """
    return [s.strip() for s in spec.split("+") if s.strip()]
