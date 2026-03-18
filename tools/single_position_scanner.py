#!/usr/bin/env python3
"""Scanner probabiliste single-position sequentiel.

Simule le comportement exact du backtest C :
- Signaux generes sur bougies 1h (agregees depuis 5m)
- Fills evalues sur bougies 5m (ordre intra-bougie exact)
- 1 position a la fois (single-position sequentiel)
- Cooldown apres chaque trade
- Walk-forward 70/30 validation

Usage:
    python3 tools/single_position_scanner.py ETH
    python3 tools/single_position_scanner.py SOL
    python3 tools/single_position_scanner.py DOGE
"""

import sys
import os
import time
import math
import itertools
import csv
from dataclasses import dataclass

import numpy as np
import pandas as pd

# Add parent to path for analyzer imports
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from analyzer.data import load_candles, aggregate_tf
from analyzer.indicators import compute_indicators
from analyzer.signals import compute_signals, SIGNAL_DEFS, scan_combinations

# ═══════════════════════════════════════════════════════════════
# Configuration par coin
# ═══════════════════════════════════════════════════════════════

COIN_CONFIG = {
    "ETH": {"leverage": 7, "equity_pct": 0.90, "max_hold": 48, "cooldown": 4, "min_trades": 20},
    "SOL": {"leverage": 5, "equity_pct": 0.70, "max_hold": 48, "cooldown": 4, "min_trades": 20},
    "DOGE": {"leverage": 3, "equity_pct": 0.50, "max_hold": 48, "cooldown": 4, "min_trades": 20},
    "BTC": {"leverage": 7, "equity_pct": 0.90, "max_hold": 48, "cooldown": 4, "min_trades": 20},
}

# Fees: maker 0.015% + taker 0.045% = 0.06% round-trip
FEES_PCT = 0.06 / 100

# Walk-forward split
WF_TRAIN_RATIO = 0.70

# TP/SL grid (TP >= SL sauf quelques asymetriques sniper)
TP_SL_GRID = [
    (1.0, 1.0), (1.5, 1.0), (1.5, 1.5), (2.0, 1.0), (2.0, 1.5), (2.0, 2.0),
    (2.5, 1.5), (2.5, 2.0), (2.5, 2.5), (3.0, 1.5), (3.0, 2.0), (3.0, 2.5),
    (3.0, 3.0), (4.0, 2.0), (4.0, 3.0), (4.0, 4.0), (5.0, 3.0), (5.0, 4.0),
    (5.0, 5.0),
    # Asymetriques (SL > TP)
    (2.0, 3.0), (3.0, 5.0), (4.0, 5.0),
]

# 5m bars per 1h candle
BARS_5M_PER_1H = 12

# Top N results to display
TOP_N = 20


# ═══════════════════════════════════════════════════════════════
# Data structures
# ═══════════════════════════════════════════════════════════════

@dataclass
class Trade:
    entry_bar: int       # index in 1h candles
    exit_bar: int        # index in 1h candles
    direction: str       # "LONG" or "SHORT"
    entry_price: float
    exit_price: float
    pnl_pct: float       # after fees, before leverage
    result: str          # "TP", "SL", "TIMEOUT"


@dataclass
class ScanResult:
    signal: str
    direction: str
    tp_pct: float
    sl_pct: float
    # In-sample
    is_trades: int
    is_wr: float
    is_return: float        # total equity return %
    is_max_dd: float        # max drawdown %
    is_monthly_growth: float
    is_trades_per_month: float
    # Out-of-sample
    oos_trades: int
    oos_wr: float
    oos_return: float
    oos_max_dd: float
    oos_monthly_growth: float
    oos_trades_per_month: float
    # Combined
    oos_pass: bool


# ═══════════════════════════════════════════════════════════════
# Core simulation engine
# ═══════════════════════════════════════════════════════════════

def simulate_single_position(
    signal_mask: np.ndarray,   # bool array on 1h candles
    candles_5m: np.ndarray,    # structured array: open, high, low, close (5m)
    candles_1h_close: np.ndarray,  # close prices of 1h candles
    n_1h: int,
    direction: str,            # "LONG" or "SHORT"
    tp_pct: float,
    sl_pct: float,
    leverage: int,
    max_hold: int,             # in 1h bars
    cooldown: int,             # in 1h bars
    bar_map_start: np.ndarray = None,  # bar_map_start[i] = first 5m index for 1h bar i
    bar_map_end: np.ndarray = None,    # bar_map_end[i] = last+1 5m index for 1h bar i
) -> list:
    """Simulate single-position sequential trading on 5m candles.

    Signals are on 1h bars. When a signal fires, we enter at 1h close
    and simulate the trade tick-by-tick on 5m candles to determine
    exact TP/SL hit order.

    Uses bar_map for timestamp-based 1h→5m mapping (no index assumption).

    Returns list of Trade objects.
    """
    trades = []
    tp_mult = tp_pct / 100.0
    sl_mult = sl_pct / 100.0
    n_5m = len(candles_5m)
    # Offset for converting global 5m index → window-relative 1h index
    first_5m = int(bar_map_start[0]) if bar_map_start is not None and len(bar_map_start) > 0 else 0
    i = 0  # 1h bar index

    while i < n_1h:
        if not signal_mask[i]:
            i += 1
            continue

        # Entry at close of signal bar
        entry_price = candles_1h_close[i]
        if entry_price <= 0:
            i += 1
            continue

        if direction == "LONG":
            tp_price = entry_price * (1 + tp_mult)
            sl_price = entry_price * (1 - sl_mult)
        else:
            tp_price = entry_price * (1 - tp_mult)
            sl_price = entry_price * (1 + sl_mult)

        # Simulate on 5m candles starting from next 1h bar
        next_bar = i + 1
        end_bar = min(i + 1 + max_hold, n_1h)

        # No room for a trade if signal fires at the last bar
        if next_bar >= n_1h:
            i += 1
            continue

        if bar_map_start is not None:
            start_5m = int(bar_map_start[next_bar])
            end_5m = int(bar_map_end[min(end_bar, n_1h) - 1]) if end_bar <= n_1h else n_5m
        else:
            start_5m = (i + 1) * BARS_5M_PER_1H
            end_5m = min((i + 1 + max_hold) * BARS_5M_PER_1H, n_5m)

        if start_5m >= n_5m:
            i += 1
            continue

        end_5m = min(end_5m, n_5m)

        result = "TIMEOUT"
        exit_price = entry_price
        exit_bar_1h = min(i + max_hold, n_1h - 1)

        for j5 in range(start_5m, end_5m):
            h = candles_5m[j5, 1]  # high
            l = candles_5m[j5, 2]  # low

            # Convert global 5m index to window-relative 1h index
            rel_1h = max(0, (j5 - first_5m) // BARS_5M_PER_1H)

            if direction == "LONG":
                if l <= sl_price:
                    result = "SL"
                    exit_price = sl_price
                    exit_bar_1h = rel_1h
                    break
                if h >= tp_price:
                    result = "TP"
                    exit_price = tp_price
                    exit_bar_1h = rel_1h
                    break
            else:  # SHORT
                if h >= sl_price:
                    result = "SL"
                    exit_price = sl_price
                    exit_bar_1h = rel_1h
                    break
                if l <= tp_price:
                    result = "TP"
                    exit_price = tp_price
                    exit_bar_1h = rel_1h
                    break

        if result == "TIMEOUT":
            last_5m = min(end_5m - 1, n_5m - 1)
            if last_5m >= start_5m:
                exit_price = candles_5m[last_5m, 3]  # close

        # Calculate PnL
        if direction == "LONG":
            pnl_pct = (exit_price - entry_price) / entry_price
        else:
            pnl_pct = (entry_price - exit_price) / entry_price

        # Subtract fees
        pnl_pct -= FEES_PCT

        trades.append(Trade(
            entry_bar=i,
            exit_bar=exit_bar_1h,
            direction=direction,
            entry_price=entry_price,
            exit_price=exit_price,
            pnl_pct=pnl_pct,
            result=result,
        ))

        # Advance cursor past trade + cooldown
        i = exit_bar_1h + 1 + cooldown

    return trades


def compute_metrics(trades: list, leverage: int, n_months: float):
    """Compute equity metrics from trade list."""
    if not trades:
        return 0, 0.0, 0.0, 0.0, 0.0, 0.0

    equity = 1.0
    peak = 1.0
    max_dd = 0.0
    wins = 0

    for t in trades:
        pnl = t.pnl_pct * leverage
        equity *= (1 + pnl)
        if equity <= 0:
            equity = 0.0001  # prevent log(0)
            max_dd = 100.0
            break
        if equity > peak:
            peak = equity
        dd = (peak - equity) / peak * 100
        if dd > max_dd:
            max_dd = dd
        if t.pnl_pct > 0:
            wins += 1

    n_trades = len(trades)
    wr = wins / n_trades * 100 if n_trades > 0 else 0
    total_return = (equity - 1) * 100

    if n_months > 0 and equity > 0:
        monthly_growth = (math.log(equity) / n_months) * 100
    else:
        monthly_growth = 0.0

    trades_per_month = n_trades / n_months if n_months > 0 else 0

    return n_trades, wr, total_return, max_dd, monthly_growth, trades_per_month


# ═══════════════════════════════════════════════════════════════
# Main scanner
# ═══════════════════════════════════════════════════════════════

def load_and_prepare(coin: str):
    """Load 5m candles, aggregate to 1h, compute indicators and signals."""
    print(f"[1/4] Chargement bougies 5m {coin}...")
    df_5m = load_candles(coin, "5m")
    n_5m = len(df_5m)
    print(f"       {n_5m:,} bougies 5m chargees")

    print(f"[2/4] Agregation 5m → 1h...")
    df_1h = aggregate_tf(df_5m, "1h")
    n_1h = len(df_1h)
    print(f"       {n_1h:,} bougies 1h")

    print(f"[3/4] Calcul indicateurs...")
    df_1h = compute_indicators(df_1h)

    print(f"[4/4] Calcul signaux ({len(SIGNAL_DEFS)} definitions)...")
    signals_df = compute_signals(df_1h)

    # Prepare 5m numpy array for fast simulation: [open, high, low, close]
    candles_5m_np = df_5m[["open", "high", "low", "close"]].values.astype(np.float64)

    # 1h close prices
    candles_1h_close = df_1h["close"].values.astype(np.float64)

    # Build timestamp-based mapping: 1h bar i → 5m index range
    # This avoids the index alignment bug (5m/1h ratio != exactly 12)
    tf_ms = 3600000  # 1h in ms
    times_5m = df_5m["time"].values.astype(np.int64)
    if times_5m.max() > 1e15:  # nanoseconds (pandas Timestamp)
        times_5m = times_5m // 10**6  # convert to ms
    times_1h = df_1h["time"].values.astype(np.int64)
    if times_1h.max() > 1e15:
        times_1h = times_1h // 10**6

    # For each 1h bar, find the range of 5m bars by timestamp
    bar_map_start = np.zeros(n_1h, dtype=np.int64)
    bar_map_end = np.zeros(n_1h, dtype=np.int64)

    # Each 1h bar covers [time_1h, time_1h + 3600000)
    # The 5m bars belonging to it have time_5m in [time_1h, time_1h + 3600000)
    j5 = 0
    for i_1h in range(n_1h):
        t_start = times_1h[i_1h]
        t_end = t_start + tf_ms

        # Advance j5 to the first 5m bar >= t_start
        while j5 < n_5m and times_5m[j5] < t_start:
            j5 += 1
        bar_map_start[i_1h] = j5

        # Find end: first 5m bar >= t_end
        j_end = j5
        while j_end < n_5m and times_5m[j_end] < t_end:
            j_end += 1
        bar_map_end[i_1h] = j_end

    n_mapped = int(bar_map_end[-1]) if n_1h > 0 else 0
    n_gaps = sum(1 for i in range(n_1h) if (bar_map_end[i] - bar_map_start[i]) != 12)

    # Time info for monthly calculations
    total_hours = n_1h
    total_months = total_hours / (24 * 30.44)  # approximate

    print(f"       Donnees alignees: {n_1h:,} bougies 1h, {n_mapped:,} bougies 5m mappees")
    print(f"       Heures avec != 12 bougies 5m: {n_gaps:,} ({n_gaps/n_1h*100:.1f}%)")
    print(f"       Periode: {total_months:.1f} mois (~{total_months/12:.1f} ans)")

    return signals_df, candles_5m_np, candles_1h_close, n_1h, total_months, bar_map_start, bar_map_end


def run_scan(coin: str):
    """Run the full single-position scan for a coin."""
    if coin not in COIN_CONFIG:
        print(f"Coin {coin} non supporte. Valides: {list(COIN_CONFIG.keys())}")
        sys.exit(1)

    cfg = COIN_CONFIG[coin]
    leverage = cfg["leverage"]
    max_hold = cfg["max_hold"]
    cooldown = cfg["cooldown"]
    min_trades = cfg["min_trades"]

    print(f"\n{'='*70}")
    print(f"  SCANNER SINGLE-POSITION — {coin} 1h")
    print(f"  Leverage: x{leverage} | Max hold: {max_hold}h | Cooldown: {cooldown}h")
    print(f"{'='*70}\n")

    signals_df, candles_5m, candles_1h_close, n_1h, total_months, bar_map_start, bar_map_end = load_and_prepare(coin)

    # Walk-forward split
    split_idx = int(n_1h * WF_TRAIN_RATIO)
    is_months = split_idx / (24 * 30.44)
    oos_months = (n_1h - split_idx) / (24 * 30.44)

    print(f"\n  Walk-forward split: IS {split_idx:,} bars ({is_months:.1f}m) | OOS {n_1h-split_idx:,} bars ({oos_months:.1f}m)")

    # Generate signal combinations (singles + doubles, min 10 occurrences for broader scan)
    print(f"\n  Generation des combinaisons de signaux...")
    combos = scan_combinations(signals_df, max_combo=2, min_occurrences=10)

    # Exclude StochRSI-based signals — too unstable between Python/C indicator computation.
    # Small RSI differences (0.3-0.5 pts) get amplified to 5-17 pts in StochRSI,
    # making threshold-based signals unreliable when transferred to C backtest.
    UNSTABLE_SIGNALS = {"stoch_os_20", "stoch_os_30", "stoch_ob_70", "stoch_ob_80"}
    combos_filtered = []
    for name, mask, count in combos:
        parts = set(name.split("+"))
        if parts & UNSTABLE_SIGNALS:
            continue
        combos_filtered.append((name, mask, count))
    n_excluded = len(combos) - len(combos_filtered)
    combos = combos_filtered
    print(f"  {len(combos):,} combinaisons ({n_excluded} exclues: StochRSI instable)")

    # Total evaluations
    n_directions = 2
    n_tpsl = len(TP_SL_GRID)
    total_evals = len(combos) * n_directions * n_tpsl
    print(f"  Total evaluations: {total_evals:,}")

    # Breakeven WR for each TP/SL pair
    def breakeven_wr(tp, sl):
        return sl / (tp + sl) * 100

    # Pre-split signal masks
    results = []
    t_start = time.time()
    eval_count = 0
    last_progress = 0

    for combo_name, combo_mask, combo_count in combos:
        for direction in ["LONG", "SHORT"]:
            for tp_pct, sl_pct in TP_SL_GRID:
                eval_count += 1

                # Progress
                progress = eval_count * 100 // total_evals
                if progress >= last_progress + 5:
                    elapsed = time.time() - t_start
                    rate = eval_count / elapsed if elapsed > 0 else 0
                    eta = (total_evals - eval_count) / rate if rate > 0 else 0
                    print(f"  [{progress:3d}%] {eval_count:,}/{total_evals:,} | {rate:.0f} eval/s | ETA {eta:.0f}s", end="\r")
                    last_progress = progress

                # --- In-sample ---
                is_mask = combo_mask[:split_idx].copy()
                is_close = candles_1h_close[:split_idx]
                # Use full 5m array with bar_map for correct alignment
                is_map_start = bar_map_start[:split_idx]
                is_map_end = bar_map_end[:split_idx]

                trades_is = simulate_single_position(
                    is_mask, candles_5m, is_close, split_idx,
                    direction, tp_pct, sl_pct, leverage, max_hold, cooldown,
                    is_map_start, is_map_end
                )

                n_is = len(trades_is)
                if n_is < min_trades:
                    continue

                n_is, wr_is, ret_is, dd_is, mg_is, tpm_is = compute_metrics(trades_is, leverage, is_months)

                # Filter: WR >= breakeven + 5pp
                be = breakeven_wr(tp_pct, sl_pct)
                if wr_is < be + 5:
                    continue

                # Filter: max DD < 50%
                if dd_is > 50:
                    continue

                # Filter: positive return
                if ret_is <= 0:
                    continue

                # Filter: min frequency
                if tpm_is < 0.5:
                    continue

                # --- Out-of-sample ---
                oos_mask = combo_mask[split_idx:n_1h].copy()
                oos_close = candles_1h_close[split_idx:n_1h]
                n_oos_1h = n_1h - split_idx
                # OOS bar_map: shift indices to be relative to oos_mask (0-based)
                # but still point to same 5m candles in the full array
                oos_map_start = bar_map_start[split_idx:n_1h]
                oos_map_end = bar_map_end[split_idx:n_1h]

                trades_oos = simulate_single_position(
                    oos_mask, candles_5m, oos_close, n_oos_1h,
                    direction, tp_pct, sl_pct, leverage, max_hold, cooldown,
                    oos_map_start, oos_map_end
                )

                n_oos, wr_oos, ret_oos, dd_oos, mg_oos, tpm_oos = compute_metrics(trades_oos, leverage, oos_months)

                # OOS pass criteria
                oos_pass = (ret_oos > 0) and (n_oos >= 5) and (wr_oos > be)

                results.append(ScanResult(
                    signal=combo_name,
                    direction=direction,
                    tp_pct=tp_pct,
                    sl_pct=sl_pct,
                    is_trades=n_is,
                    is_wr=wr_is,
                    is_return=ret_is,
                    is_max_dd=dd_is,
                    is_monthly_growth=mg_is,
                    is_trades_per_month=tpm_is,
                    oos_trades=n_oos,
                    oos_wr=wr_oos,
                    oos_return=ret_oos,
                    oos_max_dd=dd_oos,
                    oos_monthly_growth=mg_oos,
                    oos_trades_per_month=tpm_oos,
                    oos_pass=oos_pass,
                ))

    elapsed = time.time() - t_start
    print(f"\n\n  Termine: {eval_count:,} evaluations en {elapsed:.1f}s ({eval_count/elapsed:.0f}/s)")
    print(f"  {len(results):,} resultats passent les filtres IS")

    oos_passed = [r for r in results if r.oos_pass]
    print(f"  {len(oos_passed):,} passent aussi le walk-forward OOS")

    # Sort by monthly_growth IS
    results.sort(key=lambda r: r.is_monthly_growth, reverse=True)
    oos_passed.sort(key=lambda r: r.oos_monthly_growth, reverse=True)

    # Generate outputs
    output_dir = os.path.join(os.path.dirname(__file__), "..", "data", "analysis")
    os.makedirs(output_dir, exist_ok=True)

    write_csv(results, coin, output_dir)
    write_report(results, oos_passed, coin, cfg, n_1h, total_months, split_idx, is_months, oos_months, output_dir)

    print(f"\n  Fichiers generes:")
    print(f"    data/analysis/{coin}_single_pos_results.csv")
    print(f"    data/analysis/{coin}_single_pos_scan.md")


def write_csv(results: list, coin: str, output_dir: str):
    """Write raw results to CSV."""
    path = os.path.join(output_dir, f"{coin}_single_pos_results.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "signal", "direction", "tp", "sl",
            "is_trades", "is_wr", "is_return", "is_max_dd", "is_monthly_growth", "is_trades_month",
            "oos_trades", "oos_wr", "oos_return", "oos_max_dd", "oos_monthly_growth", "oos_trades_month",
            "oos_pass"
        ])
        for r in results:
            w.writerow([
                r.signal, r.direction, r.tp_pct, r.sl_pct,
                r.is_trades, f"{r.is_wr:.1f}", f"{r.is_return:.1f}", f"{r.is_max_dd:.1f}",
                f"{r.is_monthly_growth:.3f}", f"{r.is_trades_per_month:.2f}",
                r.oos_trades, f"{r.oos_wr:.1f}", f"{r.oos_return:.1f}", f"{r.oos_max_dd:.1f}",
                f"{r.oos_monthly_growth:.3f}", f"{r.oos_trades_per_month:.2f}",
                r.oos_pass
            ])


def write_report(results: list, oos_passed: list, coin: str, cfg: dict,
                 n_1h: int, total_months: float, split_idx: int,
                 is_months: float, oos_months: float, output_dir: str):
    """Write markdown report."""
    path = os.path.join(output_dir, f"{coin}_single_pos_scan.md")

    # Separate LONG and SHORT OOS-passed results
    long_pass = [r for r in oos_passed if r.direction == "LONG"]
    short_pass = [r for r in oos_passed if r.direction == "SHORT"]

    # Also top IS-only
    long_is = [r for r in results if r.direction == "LONG"][:TOP_N]
    short_is = [r for r in results if r.direction == "SHORT"][:TOP_N]

    with open(path, "w") as f:
        f.write(f"# {coin} — Single-Position Scanner Results\n\n")
        f.write(f"**Simulation**: 5m candles, signaux sur 1h agrege\n")
        f.write(f"**Data**: {n_1h:,} bougies 1h ({total_months:.1f} mois)\n")
        f.write(f"**Walk-forward**: IS {split_idx:,} bars ({is_months:.1f}m) | OOS {n_1h-split_idx:,} bars ({oos_months:.1f}m)\n")
        f.write(f"**Config**: leverage x{cfg['leverage']}, max_hold {cfg['max_hold']}h, cooldown {cfg['cooldown']}h\n")
        f.write(f"**Fees**: 0.06% round-trip\n")
        f.write(f"**Resultats**: {len(results):,} IS pass, {len(oos_passed):,} OOS pass\n\n")

        f.write(f"---\n\n")

        # OOS-passed results (the gold)
        f.write(f"## Walk-Forward PASS (IS + OOS profitable)\n\n")

        for label, subset in [("LONG", long_pass[:TOP_N]), ("SHORT", short_pass[:TOP_N])]:
            f.write(f"### Top {label}\n\n")
            if not subset:
                f.write(f"*Aucun signal {label} ne passe le walk-forward.*\n\n")
                continue

            f.write(f"| # | Signal | TP/SL | IS Trades | IS WR | IS Ret% | IS DD% | IS Growth/m | OOS Trades | OOS WR | OOS Ret% | OOS DD% | OOS Growth/m |\n")
            f.write(f"|---|--------|-------|-----------|-------|---------|--------|-------------|------------|--------|----------|---------|-------------|\n")
            for idx, r in enumerate(subset, 1):
                f.write(f"| {idx} | {r.signal} | {r.tp_pct}/{r.sl_pct} | {r.is_trades} | {r.is_wr:.1f}% | {r.is_return:.1f}% | {r.is_max_dd:.1f}% | {r.is_monthly_growth:.2f}% | {r.oos_trades} | {r.oos_wr:.1f}% | {r.oos_return:.1f}% | {r.oos_max_dd:.1f}% | {r.oos_monthly_growth:.2f}% |\n")
            f.write(f"\n")

        f.write(f"---\n\n")

        # IS-only top (for reference)
        f.write(f"## Top IS-only (reference, non valide OOS)\n\n")

        for label, subset in [("LONG", long_is), ("SHORT", short_is)]:
            f.write(f"### Top {label} (IS)\n\n")
            if not subset:
                f.write(f"*Aucun signal {label}.*\n\n")
                continue

            f.write(f"| # | Signal | TP/SL | Trades | WR | Ret% | DD% | Growth/m | Tr/m | OOS? |\n")
            f.write(f"|---|--------|-------|--------|----|------|-----|----------|------|------|\n")
            for idx, r in enumerate(subset, 1):
                oos_flag = "PASS" if r.oos_pass else "FAIL"
                f.write(f"| {idx} | {r.signal} | {r.tp_pct}/{r.sl_pct} | {r.is_trades} | {r.is_wr:.1f}% | {r.is_return:.1f}% | {r.is_max_dd:.1f}% | {r.is_monthly_growth:.2f}% | {r.is_trades_per_month:.1f} | {oos_flag} |\n")
            f.write(f"\n")

        # Signal → Lua mapping
        f.write(f"---\n\n")
        f.write(f"## Mapping Signal → Lua\n\n")
        f.write(f"```lua\n")
        f.write(f"-- Conditions a utiliser dans les strategies Lua\n")
        f.write(f"-- mid = data.mid ou close de la derniere bougie\n")
        f.write(f"-- ind = bot.get_indicators(COIN, \"1h\", 50, mid)\n\n")

        lua_map = {
            "rsi_os_30": "ind.rsi < 30",
            "rsi_os_35": "ind.rsi < 35",
            "rsi_os_40": "ind.rsi < 40",
            "rsi_os_45": "ind.rsi < 45",
            "rsi_ob_55": "ind.rsi > 55",
            "rsi_ob_60": "ind.rsi > 60",
            "rsi_ob_65": "ind.rsi > 65",
            "rsi_ob_70": "ind.rsi > 70",
            "stoch_os_20": "ind.stoch_rsi_k < 20",
            "stoch_os_30": "ind.stoch_rsi_k < 30",
            "stoch_ob_70": "ind.stoch_rsi_k > 70",
            "stoch_ob_80": "ind.stoch_rsi_k > 80",
            "bb_lower": "mid < ind.bb_lower * 1.005",
            "bb_upper": "mid > ind.bb_upper * 0.995",
            "below_bb_mid": "mid < ind.bb_middle",
            "above_bb_mid": "mid > ind.bb_middle",
            "ema_uptrend": "ind.ema_12 > ind.ema_26",
            "ema_downtrend": "ind.ema_12 < ind.ema_26",
            "adx_trending": "ind.adx_14 > 25",
            "adx_strong": "ind.adx_14 > 35",
            "adx_ranging": "ind.adx_14 < 20",
            "macd_bull": "ind.macd_histogram > 0",
            "macd_bear": "ind.macd_histogram < 0",
            "macd_cross_up": "ind.macd_histogram > 0 and prev_macd_hist <= 0",
            "macd_cross_down": "ind.macd_histogram < 0 and prev_macd_hist >= 0",
            "high_vol": "(ind.atr / mid) > 0.01",
            "low_vol": "(ind.atr / mid) < 0.005",
            "very_low_vol": "(ind.atr / mid) < 0.003",
            "cci_os": "ind.cci_20 < -100",
            "cci_ob": "ind.cci_20 > 100",
            "willr_os": "ind.williams_r < -80",
            "willr_ob": "ind.williams_r > -20",
            "obv_above_sma": "ind.obv > ind.obv_sma",
            "obv_below_sma": "ind.obv < ind.obv_sma",
            "sma20_above_sma50": "ind.sma_20 > ind.sma_50",
            "sma20_below_sma50": "ind.sma_20 < ind.sma_50",
            "above_sma200": "mid > ind.sma_200",
            "below_sma200": "mid < ind.sma_200",
            "cmf_positive": "ind.cmf > 0",
            "cmf_negative": "ind.cmf < 0",
            "cmf_strong_bull": "ind.cmf > 0.1",
            "cmf_strong_bear": "ind.cmf < -0.1",
            "mfi_os_20": "ind.mfi < 20",
            "mfi_os_30": "ind.mfi < 30",
            "mfi_ob_70": "ind.mfi > 70",
            "mfi_ob_80": "ind.mfi > 80",
            "squeeze_on": "ind.squeeze_on == 1",
            "squeeze_off": "ind.squeeze_on == 0",
            "squeeze_mom_bull": "ind.squeeze_mom > 0",
            "squeeze_mom_bear": "ind.squeeze_mom < 0",
            "di_bull": "ind.plus_di > ind.minus_di",
            "di_bear": "ind.plus_di < ind.minus_di",
            "macd_accelerating": "ind.macd_histogram > prev_macd_hist and prev_macd_hist > prev2_macd_hist",
            "macd_decelerating": "ind.macd_histogram < prev_macd_hist and prev_macd_hist < prev2_macd_hist",
        }

        for sig, lua_code in lua_map.items():
            f.write(f"-- {sig:30s} → {lua_code}\n")
        f.write(f"```\n")

    print(f"  Rapport ecrit: {path}")


# ═══════════════════════════════════════════════════════════════
# Entry point
# ═══════════════════════════════════════════════════════════════

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 tools/single_position_scanner.py <COIN>")
        print(f"  Coins supportes: {list(COIN_CONFIG.keys())}")
        sys.exit(1)

    coin = sys.argv[1].upper()
    run_scan(coin)
