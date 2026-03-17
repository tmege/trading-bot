"""Détection de régime de marché (bull/bear/neutral) pour analyse offline.

Classifie chaque bougie selon le contexte macro (tendance, force, direction)
avec hystérésis pour éviter les oscillations rapides.
"""

import numpy as np
import pandas as pd


def classify_regime(df: pd.DataFrame, hysteresis: int = 3) -> pd.Series:
    """Classifie le régime de marché pour chaque bougie.

    Règles :
    - Bull  : price > SMA200 + EMA12 > EMA26 + ADX > 20 + plus_di > minus_di
    - Bear  : price < SMA200 + EMA12 < EMA26 + ADX > 20 + minus_di > plus_di
    - Neutral : ADX < 18 ou conditions mixtes

    Hystérésis : il faut `hysteresis` bougies consécutives dans un nouveau
    régime pour confirmer le changement.

    Args:
        df: DataFrame avec indicateurs (sma_200, ema_12, ema_26, adx, plus_di, minus_di).
        hysteresis: nombre de bougies consécutives pour confirmer un changement.

    Returns:
        Series de strings 'bull', 'bear', 'neutral'.
    """
    n = len(df)
    close = df["close"].values
    sma200 = df["sma_200"].values if "sma_200" in df.columns else df["sma_50"].values
    ema12 = df["ema_12"].values
    ema26 = df["ema_26"].values
    adx = df["adx"].values
    plus_di = df["plus_di"].values
    minus_di = df["minus_di"].values

    # Raw regime sans hystérésis
    raw = np.full(n, "neutral", dtype=object)
    for i in range(n):
        if np.isnan(adx[i]) or np.isnan(sma200[i]):
            continue

        above_sma200 = close[i] > sma200[i]
        ema_bull = ema12[i] > ema26[i]
        trending = adx[i] > 20
        di_bull = plus_di[i] > minus_di[i]

        if trending and above_sma200 and ema_bull and di_bull:
            raw[i] = "bull"
        elif trending and not above_sma200 and not ema_bull and not di_bull:
            raw[i] = "bear"
        elif adx[i] < 18:
            raw[i] = "neutral"
        else:
            raw[i] = "neutral"

    # Appliquer l'hystérésis
    result = np.full(n, "neutral", dtype=object)
    current = "neutral"
    counter = 0
    pending = "neutral"

    for i in range(n):
        if raw[i] == current:
            result[i] = current
            counter = 0
            pending = current
        elif raw[i] == pending:
            counter += 1
            if counter >= hysteresis:
                current = pending
                result[i] = current
                counter = 0
            else:
                result[i] = current
        else:
            pending = raw[i]
            counter = 1
            if hysteresis <= 1:
                current = pending
                result[i] = current
                counter = 0
            else:
                result[i] = current

    return pd.Series(result, index=df.index, name="regime")


def regime_stats(df: pd.DataFrame, regime_col: str = "regime") -> pd.DataFrame:
    """Statistiques descriptives par régime.

    Returns:
        DataFrame avec colonnes : regime, count, pct, avg_duration_bars,
        avg_return_pct, avg_volatility.
    """
    regimes = df[regime_col].values
    close = df["close"].values
    atr_pct = (df["atr"] / df["close"]).values if "atr" in df.columns else np.zeros(len(df))

    stats = []
    for regime in ["bull", "bear", "neutral"]:
        mask = regimes == regime
        count = mask.sum()
        if count == 0:
            stats.append({
                "regime": regime, "count": 0, "pct": 0,
                "avg_duration_bars": 0, "avg_return_pct": 0, "avg_volatility": 0,
            })
            continue

        # Durée moyenne des séquences consécutives
        durations = _consecutive_lengths(mask)
        avg_dur = np.mean(durations) if len(durations) > 0 else 0

        # Return moyen par bougie dans ce régime
        returns = np.diff(close) / close[:-1] * 100
        regime_returns = returns[mask[1:]] if count > 1 else np.array([0])
        avg_ret = np.mean(regime_returns) if len(regime_returns) > 0 else 0

        # Volatilité moyenne
        avg_vol = np.mean(atr_pct[mask])

        stats.append({
            "regime": regime,
            "count": int(count),
            "pct": round(count / len(df) * 100, 1),
            "avg_duration_bars": round(avg_dur, 1),
            "avg_return_pct": round(avg_ret, 4),
            "avg_volatility": round(avg_vol * 100, 4),
        })

    return pd.DataFrame(stats)


def regime_transition_matrix(df: pd.DataFrame, regime_col: str = "regime") -> pd.DataFrame:
    """Matrice de transition P(regime_t+1 | regime_t).

    Returns:
        DataFrame 3x3 avec les probabilités de transition.
    """
    regimes = df[regime_col].values
    labels = ["bull", "bear", "neutral"]

    matrix = np.zeros((3, 3))
    label_to_idx = {l: i for i, l in enumerate(labels)}

    for i in range(len(regimes) - 1):
        from_r = regimes[i]
        to_r = regimes[i + 1]
        if from_r in label_to_idx and to_r in label_to_idx:
            matrix[label_to_idx[from_r]][label_to_idx[to_r]] += 1

    # Normaliser par ligne
    row_sums = matrix.sum(axis=1, keepdims=True)
    row_sums[row_sums == 0] = 1  # Éviter div/0
    matrix = matrix / row_sums

    return pd.DataFrame(matrix, index=labels, columns=labels).round(4)


def _consecutive_lengths(mask: np.ndarray) -> list:
    """Calcule les longueurs des séquences consécutives de True."""
    lengths = []
    current = 0
    for val in mask:
        if val:
            current += 1
        else:
            if current > 0:
                lengths.append(current)
            current = 0
    if current > 0:
        lengths.append(current)
    return lengths
