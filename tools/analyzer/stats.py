"""Statistiques par signal: win rate, EV, profit factor, etc."""

import numpy as np
import pandas as pd
from scipy import stats as scipy_stats


def compute_signal_stats(df_labeled: pd.DataFrame, mask: np.ndarray,
                         direction: str = "long") -> dict:
    """Calcule les statistiques d'un signal pour une direction donnée.

    Args:
        df_labeled: DataFrame avec colonnes long_result, long_pnl, short_result, short_pnl.
        mask: Boolean array indiquant les bougies où le signal est actif.
        direction: 'long' ou 'short'.

    Returns:
        Dict avec win_rate, ev, profit_factor, count, avg_win, avg_loss, ci_95, etc.
    """
    result_col = f"{direction}_result"
    pnl_col = f"{direction}_pnl"

    subset = df_labeled.loc[mask]
    count = len(subset)

    if count == 0:
        return None

    results = subset[result_col].values
    pnls = subset[pnl_col].values

    wins = results == "win"
    losses = results == "loss"
    timeouts = results == "timeout"

    n_wins = wins.sum()
    n_losses = losses.sum()
    n_timeouts = timeouts.sum()

    win_rate = n_wins / count if count > 0 else 0

    avg_win = pnls[wins].mean() if n_wins > 0 else 0
    avg_loss = abs(pnls[losses].mean()) if n_losses > 0 else 0

    total_wins = pnls[wins].sum() if n_wins > 0 else 0
    total_losses = abs(pnls[losses].sum()) if n_losses > 0 else 0

    # Expected Value: moyenne des PnL de tous les trades
    ev = pnls.mean() if count > 0 else 0

    # Profit Factor
    profit_factor = total_wins / total_losses if total_losses > 0 else (float("inf") if total_wins > 0 else 0)

    # Intervalle de confiance 95% sur le win rate (binomial)
    if count >= 5:
        ci = _wilson_ci(n_wins, count, 0.95)
    else:
        ci = (0, 1)

    # Sharpe-like ratio: ev / std(pnl)
    pnl_std = pnls.std() if count > 1 else 1
    sharpe = ev / pnl_std if pnl_std > 0 else 0

    return {
        "count": count,
        "wins": int(n_wins),
        "losses": int(n_losses),
        "timeouts": int(n_timeouts),
        "win_rate": round(win_rate * 100, 2),
        "avg_win": round(avg_win, 4),
        "avg_loss": round(avg_loss, 4),
        "ev": round(ev, 4),
        "total_pnl": round(pnls.sum(), 2),
        "profit_factor": round(profit_factor, 3),
        "sharpe": round(sharpe, 4),
        "ci_95_low": round(ci[0] * 100, 2),
        "ci_95_high": round(ci[1] * 100, 2),
    }


def _wilson_ci(successes: int, total: int, confidence: float = 0.95) -> tuple:
    """Intervalle de confiance de Wilson pour une proportion binomiale."""
    if total == 0:
        return (0, 1)
    z = scipy_stats.norm.ppf(1 - (1 - confidence) / 2)
    p_hat = successes / total
    denom = 1 + z**2 / total
    center = (p_hat + z**2 / (2 * total)) / denom
    spread = z * np.sqrt((p_hat * (1 - p_hat) + z**2 / (4 * total)) / total) / denom
    return (max(0, center - spread), min(1, center + spread))


def compute_consistency(df_labeled: pd.DataFrame, mask: np.ndarray,
                        direction: str = "long") -> dict:
    """Vérifie si le signal est consistant sur 2 sous-périodes.

    Divise les données en 2 moitiés et compare les win rates.
    """
    result_col = f"{direction}_result"
    indices = np.where(mask)[0]

    if len(indices) < 20:
        return {"consistent": None, "period1_wr": None, "period2_wr": None}

    mid = len(indices) // 2
    first_half = indices[:mid]
    second_half = indices[mid:]

    results1 = df_labeled.iloc[first_half][result_col].values
    results2 = df_labeled.iloc[second_half][result_col].values

    wr1 = (results1 == "win").sum() / len(results1) if len(results1) > 0 else 0
    wr2 = (results2 == "win").sum() / len(results2) if len(results2) > 0 else 0

    # Consistant si les deux moitiés sont du même côté de 50%, et écart < 15%
    consistent = (abs(wr1 - wr2) < 0.15) and ((wr1 > 0.45 and wr2 > 0.45) or (wr1 < 0.55 and wr2 < 0.55))

    return {
        "consistent": consistent,
        "period1_wr": round(wr1 * 100, 2),
        "period2_wr": round(wr2 * 100, 2),
    }


def compute_regime_signal_stats(df_labeled: pd.DataFrame, signal_mask: np.ndarray,
                                regime_mask: np.ndarray, direction: str = "long") -> dict:
    """Statistiques d'un signal filtré par régime.

    Args:
        df_labeled: DataFrame avec résultats de labellisation.
        signal_mask: Boolean array du signal technique.
        regime_mask: Boolean array du filtre régime (bull/bear/neutral).
        direction: 'long' ou 'short'.

    Returns:
        Dict de stats (comme compute_signal_stats) ou None si insuffisant.
    """
    combined_mask = signal_mask & regime_mask
    count = int(combined_mask.sum())
    if count < 5:
        return None

    return compute_signal_stats(df_labeled, combined_mask, direction)


def find_optimal_tp_sl(df: pd.DataFrame, mask: np.ndarray, direction: str = "long",
                        tp_grid: list = None, sl_grid: list = None,
                        max_hold_bars: int = 24) -> list:
    """Scanne une grille TP/SL pour trouver la combinaison optimale.

    Args:
        df: DataFrame avec OHLC (avant labelling).
        mask: Boolean array des signaux.
        direction: 'long' ou 'short'.
        tp_grid: Liste de TP en %.
        sl_grid: Liste de SL en %.
        max_hold_bars: Max holding period.

    Returns:
        Liste de dicts triés par EV décroissant.
    """
    from .labeling import label_trades_vectorized

    if tp_grid is None:
        tp_grid = [0.5, 0.8, 1.0, 1.2, 1.5, 2.0, 3.0]
    if sl_grid is None:
        sl_grid = [1.5, 2.0, 3.0, 4.0, 5.0, 6.0]

    results = []
    for tp in tp_grid:
        for sl in sl_grid:
            labeled = label_trades_vectorized(df, tp, sl, max_hold_bars)
            stats = compute_signal_stats(labeled, mask, direction)
            if stats and stats["count"] >= 10:
                results.append({
                    "tp": tp,
                    "sl": sl,
                    "rr": round(tp / sl, 2),
                    **stats,
                })

    results.sort(key=lambda x: x["ev"], reverse=True)
    return results
