#!/usr/bin/env python3
"""Regime Analyzer — Pipeline d'analyse per-régime avec walk-forward et Monte Carlo.

Identifie les meilleurs signaux par régime de marché (bull/bear/neutral)
et valide leur robustesse via walk-forward et simulation Monte Carlo.

Usage:
    python3 tools/regime_analyzer.py --coin ETH,SOL --tf 1h
    python3 tools/regime_analyzer.py --coin ETH --tf 1h --validate --montecarlo --capital 672
"""

import argparse
import os
import sys
import time
from datetime import datetime

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from analyzer.data import load_candles, aggregate_tf, available_coins, candle_count
from analyzer.indicators import compute_indicators
from analyzer.labeling import label_trades_vectorized
from analyzer.signals import compute_signals, scan_combinations, SIGNAL_DEFS
from analyzer.stats import compute_signal_stats, compute_regime_signal_stats, find_optimal_tp_sl
from analyzer.regime import classify_regime, regime_stats, regime_transition_matrix
from analyzer.walk_forward import walk_forward_validate
from analyzer.monte_carlo import monte_carlo_equity, regime_transition_mc

REPORT_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "data", "analysis"))

# Grille TP/SL étendue pour le scan per-régime
TP_GRID = [1.0, 1.5, 2.0, 3.0, 4.0, 5.0]
SL_GRID = [2.0, 3.0, 4.0, 5.0]

DEFAULT_MAX_HOLD = 24
DEFAULT_MIN_OCC = 20  # Plus bas que l'analyzer standard (30) car filtré par régime


def analyze_regime_coin(coin: str, tf: str, do_validate: bool = False,
                        do_montecarlo: bool = False, capital: float = 672,
                        max_hold: int = DEFAULT_MAX_HOLD, min_occ: int = DEFAULT_MIN_OCC):
    """Analyse complète per-régime d'un coin."""

    print(f"\n{'='*70}")
    print(f"  REGIME ANALYZER : {coin} {tf}")
    print(f"{'='*70}")

    # 1. Load & aggregate
    t0 = time.time()
    print(f"\n[1/8] Chargement des bougies 5m...", end=" ", flush=True)
    df_5m = load_candles(coin, "5m")
    print(f"{len(df_5m):,} bougies ({time.time()-t0:.1f}s)")

    if len(df_5m) < 5000:
        print(f"  ERREUR: pas assez de données ({len(df_5m)}). Min: 5000.")
        return

    date_range = f"{df_5m['time'].iloc[0].strftime('%Y-%m-%d')} → {df_5m['time'].iloc[-1].strftime('%Y-%m-%d')}"
    print(f"  Période : {date_range}")

    t0 = time.time()
    print(f"\n[2/8] Agrégation 5m → {tf}...", end=" ", flush=True)
    df = aggregate_tf(df_5m, tf)
    print(f"{len(df):,} bougies {tf} ({time.time()-t0:.1f}s)")

    if len(df) < 500:
        print(f"  ERREUR: pas assez de bougies ({len(df)}). Min: 500.")
        return

    # 2. Indicators
    t0 = time.time()
    print(f"\n[3/8] Calcul des indicateurs...", end=" ", flush=True)
    df = compute_indicators(df)
    # Supprimer NaN (SMA200 a besoin de 200 bars)
    valid_from = df["sma_200"].first_valid_index()
    if valid_from is not None:
        df = df.loc[valid_from:].reset_index(drop=True)
    print(f"{len(df):,} bougies valides ({time.time()-t0:.1f}s)")

    if len(df) < 300:
        print(f"  ERREUR: pas assez de bougies après trim ({len(df)}).")
        return

    # 3. Classify regime
    t0 = time.time()
    print(f"\n[4/8] Classification des régimes...", end=" ", flush=True)
    df["regime"] = classify_regime(df)
    print(f"({time.time()-t0:.1f}s)")

    # Afficher les stats de régime
    rstats = regime_stats(df)
    print(f"\n  Distribution des régimes:")
    for _, row in rstats.iterrows():
        print(f"    {row['regime']:>8s}: {row['count']:>5d} bougies ({row['pct']:>5.1f}%), "
              f"durée moy={row['avg_duration_bars']:.0f} bars, vol={row['avg_volatility']:.3f}%")

    rtm = regime_transition_matrix(df)
    print(f"\n  Matrice de transition:")
    print(f"    {'':>10s} → bull    → bear    → neutral")
    for regime in ["bull", "bear", "neutral"]:
        row = rtm.loc[regime]
        print(f"    {regime:>10s}   {row['bull']:.3f}    {row['bear']:.3f}    {row['neutral']:.3f}")

    # 4. Compute signals (avec les colonnes de régime)
    t0 = time.time()
    print(f"\n[5/8] Scan des signaux...", end=" ", flush=True)
    signals_df = compute_signals(df)
    combos = scan_combinations(signals_df, max_combo=3, min_occurrences=min_occ)
    print(f"{len(combos)} combinaisons ({time.time()-t0:.1f}s)")

    # 5. Per-regime stats
    t0 = time.time()
    print(f"\n[6/8] Statistiques per-régime...", end=" ", flush=True)

    regimes_to_scan = ["bull", "bear", "neutral"]
    regime_masks = {}
    for r in regimes_to_scan:
        regime_masks[r] = (df["regime"].values == r)

    # Pour chaque régime × direction × signal, trouver les meilleurs
    all_results = {}  # {(regime, direction): [(name, stats, tp_sl_scan)]}

    for regime in regimes_to_scan:
        r_mask = regime_masks[regime]
        r_count = int(r_mask.sum())
        if r_count < 50:
            print(f"\n  {regime}: trop peu de bougies ({r_count}), skip")
            continue

        for direction in ["long", "short"]:
            key = (regime, direction)
            results = []

            # Labelliser avec une grille TP/SL de base
            for tp in TP_GRID:
                for sl in SL_GRID:
                    labeled = label_trades_vectorized(df, tp, sl, max_hold)

                    for name, mask, count in combos:
                        combined = mask & r_mask
                        combined_count = int(combined.sum())
                        if combined_count < min_occ:
                            continue

                        stats = compute_signal_stats(labeled, combined, direction)
                        if stats and stats["ev"] > 0 and stats["profit_factor"] > 1.0:
                            results.append({
                                "name": name,
                                "tp": tp,
                                "sl": sl,
                                "stats": stats,
                                "mask": combined,
                            })

            # Dédupliquer : garder le meilleur TP/SL par signal
            best_by_signal = {}
            for r in results:
                key_name = r["name"]
                if key_name not in best_by_signal or r["stats"]["ev"] > best_by_signal[key_name]["stats"]["ev"]:
                    best_by_signal[key_name] = r

            # Trier par EV
            sorted_results = sorted(best_by_signal.values(), key=lambda x: x["stats"]["ev"], reverse=True)
            all_results[key] = sorted_results[:15]  # Top 15 par régime×direction

    print(f"({time.time()-t0:.1f}s)")

    # Afficher les résultats
    for (regime, direction), results in sorted(all_results.items()):
        if not results:
            continue
        print(f"\n  Top {regime.upper()} {direction.upper()}:")
        print(f"  {'Signal':<45} {'TP/SL':>8} {'Count':>6} {'WR%':>6} {'EV%':>8} {'PF':>6}")
        print(f"  {'-'*45} {'-'*8} {'-'*6} {'-'*6} {'-'*8} {'-'*6}")
        for r in results[:10]:
            s = r["stats"]
            pf_str = f"{s['profit_factor']:.2f}" if s['profit_factor'] < 100 else "∞"
            print(f"  {r['name']:<45} {r['tp']:.0f}/{r['sl']:.0f} {s['count']:>6} "
                  f"{s['win_rate']:>5.1f}% {s['ev']:>7.3f}% {pf_str:>6}")

    # 6. Walk-forward validation (si demandé)
    wf_results = {}
    if do_validate:
        t0 = time.time()
        print(f"\n[7/8] Walk-forward validation...", flush=True)

        for (regime, direction), results in all_results.items():
            for r in results[:5]:  # Top 5 par régime×direction
                label = f"{regime}_{direction}_{r['name']}_TP{r['tp']}_SL{r['sl']}"
                print(f"    {label}...", end=" ", flush=True)
                wf = walk_forward_validate(
                    df, r["mask"], direction,
                    tp_pct=r["tp"], sl_pct=r["sl"],
                    max_hold_bars=max_hold, n_splits=5,
                )
                wf_results[label] = wf
                if wf["valid"]:
                    status = "ROBUST" if wf["robust"] else "FRAGILE"
                    print(f"OOS_EV={wf['oos_ev']:.3f}% deg={wf['degradation_pct']:.0f}% → {status}")
                else:
                    print(f"SKIP ({wf.get('reason', 'N/A')})")

        print(f"  Walk-forward terminé ({time.time()-t0:.1f}s)")
    else:
        print(f"\n[7/8] Walk-forward : SKIP (--validate pour activer)")

    # 7. Monte Carlo (si demandé)
    mc_results = {}
    if do_montecarlo:
        t0 = time.time()
        print(f"\n[8/8] Monte Carlo simulation...", flush=True)

        # Collecter les PnL par régime
        regime_pnl = {}
        regime_dur = {}

        for (regime, direction), results in all_results.items():
            if not results:
                continue
            # Prendre les top 3 signaux et agréger les PnL
            all_pnl = []
            for r in results[:3]:
                labeled = label_trades_vectorized(df, r["tp"], r["sl"], max_hold)
                pnl_col = f"{direction}_pnl"
                pnls = labeled.loc[r["mask"]][pnl_col].values
                all_pnl.extend(pnls)

            if len(all_pnl) >= 10:
                rkey = regime
                if rkey not in regime_pnl:
                    regime_pnl[rkey] = []
                regime_pnl[rkey].extend(all_pnl)

        # Durées moyennes
        for regime in regimes_to_scan:
            rstats_row = rstats[rstats["regime"] == regime]
            if len(rstats_row) > 0:
                regime_dur[regime] = float(rstats_row["avg_duration_bars"].iloc[0])

        # MC simple (tous les trades poolés)
        all_trades_pnl = []
        for pnls in regime_pnl.values():
            all_trades_pnl.extend(pnls)

        if len(all_trades_pnl) >= 20:
            mc = monte_carlo_equity(
                np.array(all_trades_pnl), n_sims=10000,
                capital=capital, leverage=5.0,
            )
            mc_results["global"] = mc
            print(f"  Global MC: P(ruine)={mc['p_ruin']:.1f}%, "
                  f"P95 DD={mc['p95_drawdown']:.1f}%, "
                  f"median return={mc['median_return']:.1f}%")

        # MC per-régime
        regime_trades_arrays = {r: np.array(p) for r, p in regime_pnl.items() if len(p) >= 10}
        if regime_trades_arrays and regime_dur:
            mc_regime = regime_transition_mc(
                regime_trades_arrays, regime_dur,
                n_sims=5000, n_periods=8760,  # 1 an en bougies 1h
                capital=capital,
            )
            mc_results["regime_mc"] = mc_regime
            print(f"  Regime MC: P(ruine)={mc_regime['p_ruin']:.1f}%, "
                  f"median return={mc_regime['median_return']:.1f}%")

        print(f"  Monte Carlo terminé ({time.time()-t0:.1f}s)")
    else:
        print(f"\n[8/8] Monte Carlo : SKIP (--montecarlo pour activer)")

    # 8. Generate report
    filepath = _generate_regime_report(
        coin, tf, df, rstats, rtm, all_results,
        wf_results, mc_results, date_range,
    )
    print(f"\n  Rapport : {filepath}")


def _generate_regime_report(coin, tf, df, rstats, rtm, all_results,
                            wf_results, mc_results, date_range):
    """Génère le rapport markdown per-régime."""
    os.makedirs(REPORT_DIR, exist_ok=True)

    lines = []
    lines.append(f"# Analyse Per-Régime — {coin} {tf}")
    lines.append(f"")
    lines.append(f"*Généré le {datetime.now().strftime('%Y-%m-%d %H:%M')}*")
    lines.append(f"")
    lines.append(f"- **Coin** : {coin}")
    lines.append(f"- **Timeframe** : {tf}")
    lines.append(f"- **Bougies analysées** : {len(df):,}")
    lines.append(f"- **Période** : {date_range}")
    lines.append(f"- **Fees** : 0.06% round-trip")
    lines.append(f"")

    # Régime distribution
    lines.append(f"## Distribution des Régimes")
    lines.append(f"")
    lines.append(f"| Régime | Bougies | % | Durée moy | Vol moy |")
    lines.append(f"|--------|---------|---|-----------|---------|")
    for _, row in rstats.iterrows():
        lines.append(f"| {row['regime']} | {row['count']} | {row['pct']}% | "
                     f"{row['avg_duration_bars']:.0f} bars | {row['avg_volatility']:.3f}% |")
    lines.append(f"")

    # Transition matrix
    lines.append(f"## Matrice de Transition")
    lines.append(f"")
    lines.append(f"| De \\ Vers | bull | bear | neutral |")
    lines.append(f"|-----------|------|------|---------|")
    for regime in ["bull", "bear", "neutral"]:
        row = rtm.loc[regime]
        lines.append(f"| {regime} | {row['bull']:.3f} | {row['bear']:.3f} | {row['neutral']:.3f} |")
    lines.append(f"")

    # Top signaux par régime
    for (regime, direction), results in sorted(all_results.items()):
        if not results:
            continue
        lines.append(f"## Top Signaux — {regime.upper()} / {direction.upper()}")
        lines.append(f"")
        lines.append(f"| # | Signal | TP/SL | Count | WR% | EV% | PF | Sharpe |")
        lines.append(f"|---|--------|-------|-------|-----|-----|----|--------|")

        for i, r in enumerate(results[:10], 1):
            s = r["stats"]
            pf_str = f"{s['profit_factor']:.2f}" if s['profit_factor'] < 100 else "∞"
            lines.append(f"| {i} | `{r['name']}` | {r['tp']:.0f}/{r['sl']:.0f} | "
                         f"{s['count']} | {s['win_rate']:.1f} | {s['ev']:.3f} | "
                         f"{pf_str} | {s['sharpe']:.3f} |")
        lines.append(f"")

    # Walk-forward results
    if wf_results:
        lines.append(f"## Walk-Forward Validation")
        lines.append(f"")
        lines.append(f"| Signal | OOS EV% | OOS WR% | OOS PF | Dégradation | Statut |")
        lines.append(f"|--------|---------|---------|--------|-------------|--------|")
        for label, wf in sorted(wf_results.items()):
            if not wf["valid"]:
                lines.append(f"| `{label}` | — | — | — | — | {wf.get('reason', 'N/A')} |")
                continue
            status = "ROBUST" if wf["robust"] else "FRAGILE"
            lines.append(f"| `{label}` | {wf['oos_ev']:.3f} | {wf['oos_wr']:.1f} | "
                         f"{wf['oos_pf']:.2f} | {wf['degradation_pct']:.0f}% | {status} |")
        lines.append(f"")

    # Monte Carlo
    if mc_results:
        lines.append(f"## Monte Carlo Simulation")
        lines.append(f"")
        for mc_name, mc in mc_results.items():
            if not mc.get("valid", False):
                continue
            lines.append(f"### {mc_name}")
            lines.append(f"")
            lines.append(f"- **Simulations** : {mc.get('n_sims', 0):,}")
            lines.append(f"- **P(ruine)** : {mc['p_ruin']:.2f}%")
            lines.append(f"- **P95 Drawdown** : {mc['p95_drawdown']:.1f}%")
            lines.append(f"- **Rendement médian** : {mc['median_return']:.1f}%")
            lines.append(f"- **Rendement P5/P95** : {mc['p5_return']:.1f}% / {mc['p95_return']:.1f}%")
            if "median_final_equity" in mc:
                lines.append(f"- **Equity finale médiane** : ${mc['median_final_equity']:.0f}")
            lines.append(f"")

    # Recommandations
    lines.append(f"## Recommandations pour Stratégie Lua")
    lines.append(f"")

    for (regime, direction), results in sorted(all_results.items()):
        strong = [r for r in results if r["stats"]["ev"] > 0.8 and r["stats"]["win_rate"] > 55]
        if not strong:
            continue
        lines.append(f"### {regime.upper()} — {direction.upper()}")
        lines.append(f"")
        for r in strong[:3]:
            s = r["stats"]
            wf_label = f"{regime}_{direction}_{r['name']}_TP{r['tp']}_SL{r['sl']}"
            wf = wf_results.get(wf_label, {})
            robust_str = ""
            if wf.get("valid"):
                robust_str = f" — WF: {'ROBUST' if wf['robust'] else 'FRAGILE'} (deg {wf['degradation_pct']:.0f}%)"
            lines.append(f"- **`{r['name']}`** TP={r['tp']}%/SL={r['sl']}% "
                         f"(EV={s['ev']:.3f}%, WR={s['win_rate']:.1f}%, PF={s['profit_factor']:.2f})"
                         f"{robust_str}")
        lines.append(f"")

    content = "\n".join(lines)
    filepath = os.path.join(REPORT_DIR, f"{coin}_{tf}_regime_report.md")
    with open(filepath, "w") as f:
        f.write(content)

    return filepath


def main():
    parser = argparse.ArgumentParser(
        description="Analyse per-régime avec walk-forward et Monte Carlo",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Exemples:
  python3 tools/regime_analyzer.py --coin ETH,SOL --tf 1h
  python3 tools/regime_analyzer.py --coin ETH --tf 1h --validate --montecarlo
  python3 tools/regime_analyzer.py --coin ETH --tf 1h --validate --montecarlo --capital 672
        """,
    )

    parser.add_argument("--coin", required=True, help="Coin(s) à analyser (ex: ETH ou ETH,SOL)")
    parser.add_argument("--tf", default="1h", help="Timeframe. Défaut: 1h")
    parser.add_argument("--validate", action="store_true", help="Activer walk-forward validation")
    parser.add_argument("--montecarlo", action="store_true", help="Activer Monte Carlo simulation")
    parser.add_argument("--capital", type=float, default=672, help="Capital pour Monte Carlo. Défaut: 672")
    parser.add_argument("--max-hold", type=int, default=DEFAULT_MAX_HOLD, help="Max holding period")
    parser.add_argument("--min-occ", type=int, default=DEFAULT_MIN_OCC, help="Min occurrences par signal")
    parser.add_argument("--list-coins", action="store_true", help="Liste les coins disponibles")

    args = parser.parse_args()

    if args.list_coins:
        for c in available_coins():
            cnt = candle_count(c, "5m")
            print(f"  {c}: {cnt:,} bougies 5m")
        return

    coins = [c.strip().upper() for c in args.coin.split(",")]

    print(f"Regime Analyzer — Analyse Per-Régime")
    print(f"Config: TF={args.tf}, max_hold={args.max_hold}, min_occ={args.min_occ}")
    print(f"Validate={'oui' if args.validate else 'non'}, MC={'oui' if args.montecarlo else 'non'}")

    t_start = time.time()

    for coin in coins:
        analyze_regime_coin(
            coin=coin, tf=args.tf,
            do_validate=args.validate,
            do_montecarlo=args.montecarlo,
            capital=args.capital,
            max_hold=args.max_hold,
            min_occ=args.min_occ,
        )

    elapsed = time.time() - t_start
    print(f"\n{'='*70}")
    print(f"  Terminé en {elapsed:.1f}s")
    print(f"{'='*70}")


if __name__ == "__main__":
    main()
