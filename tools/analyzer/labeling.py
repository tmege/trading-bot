"""Labellisation forward-looking des trades.

Pour chaque bougie, simule un trade LONG et SHORT avec TP/SL donnés,
en regardant dans le futur pour déterminer win/loss.
"""

import numpy as np
import pandas as pd


def label_trades(df: pd.DataFrame, tp_pct: float, sl_pct: float,
                 max_hold_bars: int = 24) -> pd.DataFrame:
    """Labellise chaque bougie avec le résultat d'un trade hypothétique.

    Args:
        df: DataFrame avec colonnes open, high, low, close.
        tp_pct: Take-profit en % (ex: 1.2 pour 1.2%).
        sl_pct: Stop-loss en % (ex: 4.0 pour 4.0%).
        max_hold_bars: Nombre max de bougies à regarder dans le futur.

    Returns:
        DataFrame avec colonnes ajoutées:
        - long_result: 'win', 'loss', 'timeout'
        - short_result: 'win', 'loss', 'timeout'
        - long_pnl: PnL en % (après fees)
        - short_pnl: PnL en % (après fees)
    """
    tp = tp_pct / 100.0
    sl = sl_pct / 100.0
    fees = 0.0006  # 0.06% round-trip (maker entry + taker exit)

    n = len(df)
    closes = df["close"].values
    highs = df["high"].values
    lows = df["low"].values

    long_result = np.full(n, "timeout", dtype=object)
    short_result = np.full(n, "timeout", dtype=object)
    long_pnl = np.zeros(n)
    short_pnl = np.zeros(n)

    for i in range(n - 1):
        entry = closes[i]
        long_tp_price = entry * (1 + tp)
        long_sl_price = entry * (1 - sl)
        short_tp_price = entry * (1 - tp)
        short_sl_price = entry * (1 + sl)

        end_idx = min(i + 1 + max_hold_bars, n)

        # --- LONG ---
        long_done = False
        for j in range(i + 1, end_idx):
            hit_tp = highs[j] >= long_tp_price
            hit_sl = lows[j] <= long_sl_price
            if hit_tp and hit_sl:
                # Même bougie: open → low d'abord si open plus proche du SL
                # Heuristique: si open est plus bas que mid → SL touché d'abord
                mid = (long_tp_price + long_sl_price) / 2
                if closes[j - 1] < mid if j > 0 else entry < mid:
                    long_result[i] = "loss"
                    long_pnl[i] = -sl_pct - fees * 100
                else:
                    long_result[i] = "win"
                    long_pnl[i] = tp_pct - fees * 100
                long_done = True
                break
            elif hit_tp:
                long_result[i] = "win"
                long_pnl[i] = tp_pct - fees * 100
                long_done = True
                break
            elif hit_sl:
                long_result[i] = "loss"
                long_pnl[i] = -sl_pct - fees * 100
                long_done = True
                break
        if not long_done:
            # Timeout: PnL = différence de prix à la sortie
            if end_idx > i + 1:
                exit_price = closes[min(end_idx - 1, n - 1)]
                long_pnl[i] = ((exit_price - entry) / entry * 100) - fees * 100

        # --- SHORT ---
        short_done = False
        for j in range(i + 1, end_idx):
            hit_tp = lows[j] <= short_tp_price
            hit_sl = highs[j] >= short_sl_price
            if hit_tp and hit_sl:
                mid = (short_tp_price + short_sl_price) / 2
                if closes[j - 1] > mid if j > 0 else entry > mid:
                    short_result[i] = "loss"
                    short_pnl[i] = -sl_pct - fees * 100
                else:
                    short_result[i] = "win"
                    short_pnl[i] = tp_pct - fees * 100
                short_done = True
                break
            elif hit_tp:
                short_result[i] = "win"
                short_pnl[i] = tp_pct - fees * 100
                short_done = True
                break
            elif hit_sl:
                short_result[i] = "loss"
                short_pnl[i] = -sl_pct - fees * 100
                short_done = True
                break
        if not short_done:
            if end_idx > i + 1:
                exit_price = closes[min(end_idx - 1, n - 1)]
                short_pnl[i] = ((entry - exit_price) / entry * 100) - fees * 100

    df = df.copy()
    df["long_result"] = long_result
    df["short_result"] = short_result
    df["long_pnl"] = long_pnl
    df["short_pnl"] = short_pnl
    return df


def label_trades_vectorized(df: pd.DataFrame, tp_pct: float, sl_pct: float,
                            max_hold_bars: int = 24) -> pd.DataFrame:
    """Version optimisée NumPy de label_trades.

    Utilise des rolling max/min pour accélérer le scan.
    """
    tp = tp_pct / 100.0
    sl = sl_pct / 100.0
    fees_pct = 0.06  # round-trip fees en %

    n = len(df)
    closes = df["close"].values
    highs = df["high"].values
    lows = df["low"].values

    long_result = np.full(n, 2, dtype=np.int8)   # 0=win, 1=loss, 2=timeout
    short_result = np.full(n, 2, dtype=np.int8)
    long_pnl = np.zeros(n, dtype=np.float64)
    short_pnl = np.zeros(n, dtype=np.float64)

    # Précalculer pour chaque bar i, le premier bar j dans [i+1, i+max_hold]
    # où high >= entry*(1+tp) et où low <= entry*(1-sl)
    for i in range(n - 1):
        entry = closes[i]
        end_idx = min(i + 1 + max_hold_bars, n)
        window_highs = highs[i + 1:end_idx]
        window_lows = lows[i + 1:end_idx]

        if len(window_highs) == 0:
            continue

        # --- LONG ---
        long_tp_price = entry * (1 + tp)
        long_sl_price = entry * (1 - sl)
        tp_hits = np.where(window_highs >= long_tp_price)[0]
        sl_hits = np.where(window_lows <= long_sl_price)[0]

        first_tp = tp_hits[0] if len(tp_hits) > 0 else max_hold_bars + 1
        first_sl = sl_hits[0] if len(sl_hits) > 0 else max_hold_bars + 1

        if first_tp < first_sl:
            long_result[i] = 0
            long_pnl[i] = tp_pct - fees_pct
        elif first_sl < first_tp:
            long_result[i] = 1
            long_pnl[i] = -sl_pct - fees_pct
        elif first_tp == first_sl and first_tp <= max_hold_bars:
            # Même bougie: heuristique basée sur la position du close précédent
            mid = (long_tp_price + long_sl_price) / 2
            prev_close = closes[i]
            if prev_close < mid:
                long_result[i] = 1
                long_pnl[i] = -sl_pct - fees_pct
            else:
                long_result[i] = 0
                long_pnl[i] = tp_pct - fees_pct
        else:
            # Timeout
            exit_price = closes[end_idx - 1]
            long_pnl[i] = ((exit_price - entry) / entry * 100) - fees_pct

        # --- SHORT ---
        short_tp_price = entry * (1 - tp)
        short_sl_price = entry * (1 + sl)
        tp_hits_s = np.where(window_lows <= short_tp_price)[0]
        sl_hits_s = np.where(window_highs >= short_sl_price)[0]

        first_tp_s = tp_hits_s[0] if len(tp_hits_s) > 0 else max_hold_bars + 1
        first_sl_s = sl_hits_s[0] if len(sl_hits_s) > 0 else max_hold_bars + 1

        if first_tp_s < first_sl_s:
            short_result[i] = 0
            short_pnl[i] = tp_pct - fees_pct
        elif first_sl_s < first_tp_s:
            short_result[i] = 1
            short_pnl[i] = -sl_pct - fees_pct
        elif first_tp_s == first_sl_s and first_tp_s <= max_hold_bars:
            mid = (short_tp_price + short_sl_price) / 2
            prev_close = closes[i]
            if prev_close > mid:
                short_result[i] = 1
                short_pnl[i] = -sl_pct - fees_pct
            else:
                short_result[i] = 0
                short_pnl[i] = tp_pct - fees_pct
        else:
            exit_price = closes[end_idx - 1]
            short_pnl[i] = ((entry - exit_price) / entry * 100) - fees_pct

    result_map = {0: "win", 1: "loss", 2: "timeout"}
    df = df.copy()
    df["long_result"] = pd.Categorical([result_map[r] for r in long_result])
    df["short_result"] = pd.Categorical([result_map[r] for r in short_result])
    df["long_pnl"] = long_pnl
    df["short_pnl"] = short_pnl
    return df
