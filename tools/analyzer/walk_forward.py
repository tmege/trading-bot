"""Walk-forward validation pour éviter l'overfitting.

Divise les données en N splits (train 70% / test 30%) et mesure
la dégradation des métriques hors-échantillon.
"""

import numpy as np
import pandas as pd
from .labeling import label_trades_vectorized
from .stats import compute_signal_stats


def walk_forward_validate(df: pd.DataFrame, signal_mask: np.ndarray,
                          direction: str = "long", tp_pct: float = 2.0,
                          sl_pct: float = 3.0, max_hold_bars: int = 24,
                          n_splits: int = 5, train_ratio: float = 0.7) -> dict:
    """Walk-forward validation d'un signal.

    Découpe les données en `n_splits` fenêtres glissantes.
    Chaque fenêtre : train (70%) → calibration, test (30%) → validation OOS.

    Args:
        df: DataFrame avec OHLC et indicateurs.
        signal_mask: Boolean array des signaux.
        direction: 'long' ou 'short'.
        tp_pct: Take-profit en %.
        sl_pct: Stop-loss en %.
        max_hold_bars: Max holding period.
        n_splits: Nombre de splits.
        train_ratio: Proportion train vs total.

    Returns:
        Dict avec :
        - is_ev, is_wr, is_pf : métriques in-sample moyennes
        - oos_ev, oos_wr, oos_pf : métriques out-of-sample moyennes
        - degradation_pct : (is_ev - oos_ev) / is_ev * 100
        - splits : détails par split
        - robust : True si dégradation < 30%
    """
    n = len(df)
    if n < 200:
        return _empty_result("Pas assez de données")

    total_signals = int(signal_mask.sum())
    if total_signals < 30:
        return _empty_result(f"Seulement {total_signals} signaux")

    # Taille de chaque fenêtre (train + test)
    step = n // (n_splits + 1)
    window_size = int(n * 0.6)  # 60% du dataset par fenêtre

    if window_size < 200 or step < 50:
        return _empty_result("Dataset trop petit pour walk-forward")

    splits = []

    for i in range(n_splits):
        start = i * step
        end = min(start + window_size, n)

        if end - start < 100:
            continue

        train_end = start + int((end - start) * train_ratio)

        # Labelliser
        window_df = df.iloc[start:end].copy()
        window_labeled = label_trades_vectorized(window_df, tp_pct, sl_pct, max_hold_bars)
        window_mask = signal_mask[start:end]

        # Split train/test
        train_len = train_end - start
        train_mask = np.zeros(len(window_df), dtype=bool)
        train_mask[:train_len] = window_mask[:train_len]

        test_mask = np.zeros(len(window_df), dtype=bool)
        test_mask[train_len:] = window_mask[train_len:]

        train_count = int(train_mask.sum())
        test_count = int(test_mask.sum())

        if train_count < 10 or test_count < 5:
            continue

        train_stats = compute_signal_stats(window_labeled, train_mask, direction)
        test_stats = compute_signal_stats(window_labeled, test_mask, direction)

        if train_stats is None or test_stats is None:
            continue

        splits.append({
            "split": i + 1,
            "train_range": f"{start}-{train_end}",
            "test_range": f"{train_end}-{end}",
            "train_count": train_count,
            "test_count": test_count,
            "train_ev": train_stats["ev"],
            "train_wr": train_stats["win_rate"],
            "train_pf": train_stats["profit_factor"],
            "test_ev": test_stats["ev"],
            "test_wr": test_stats["win_rate"],
            "test_pf": test_stats["profit_factor"],
        })

    if len(splits) < 2:
        return _empty_result(f"Seulement {len(splits)} splits valides")

    # Moyennes
    is_ev = np.mean([s["train_ev"] for s in splits])
    is_wr = np.mean([s["train_wr"] for s in splits])
    is_pf = np.mean([s["train_pf"] for s in splits])
    oos_ev = np.mean([s["test_ev"] for s in splits])
    oos_wr = np.mean([s["test_wr"] for s in splits])
    oos_pf = np.mean([s["test_pf"] for s in splits])

    # Dégradation
    degradation = ((is_ev - oos_ev) / abs(is_ev) * 100) if is_ev != 0 else 0

    return {
        "valid": True,
        "n_splits": len(splits),
        "is_ev": round(is_ev, 4),
        "is_wr": round(is_wr, 2),
        "is_pf": round(is_pf, 3),
        "oos_ev": round(oos_ev, 4),
        "oos_wr": round(oos_wr, 2),
        "oos_pf": round(oos_pf, 3),
        "degradation_pct": round(degradation, 1),
        "robust": degradation < 30,
        "splits": splits,
    }


def _empty_result(reason: str) -> dict:
    """Résultat vide quand la validation n'est pas possible."""
    return {
        "valid": False,
        "reason": reason,
        "n_splits": 0,
        "is_ev": 0, "is_wr": 0, "is_pf": 0,
        "oos_ev": 0, "oos_wr": 0, "oos_pf": 0,
        "degradation_pct": 0,
        "robust": False,
        "splits": [],
    }
