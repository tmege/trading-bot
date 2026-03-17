#!/usr/bin/env python3
"""Strategy Analyzer — Pipeline d'analyse probabiliste.

Analyse statistiquement les données historiques pour identifier les signaux
d'entrée avec la meilleure probabilité de succès.

Usage:
    python3 tools/strategy_analyzer.py --coin ETH --tf 1h
    python3 tools/strategy_analyzer.py --coin ETH,SOL --tf 1h
    python3 tools/strategy_analyzer.py --coin ETH --tf 1h --signal "ema_uptrend+rsi_os_40+stoch_os_30"
    python3 tools/strategy_analyzer.py --coin SOL --tf 1h --tp 0.8,1.0,1.2,1.5 --sl 3.0,4.0,5.0
"""

import argparse
import sys
import time
import os

# Ajouter le répertoire parent au path pour les imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from analyzer.data import load_candles, aggregate_tf, available_coins, candle_count
from analyzer.indicators import compute_indicators
from analyzer.labeling import label_trades_vectorized
from analyzer.signals import compute_signals, scan_combinations, parse_signal_spec, SIGNAL_DEFS
from analyzer.stats import compute_signal_stats, compute_consistency, find_optimal_tp_sl
from analyzer.report import generate_report


DEFAULT_TP = 1.2
DEFAULT_SL = 4.0
DEFAULT_MAX_HOLD = 24
DEFAULT_MIN_OCC = 30
DEFAULT_MAX_COMBO = 3
TOP_N_FOR_TP_SL_SCAN = 5

TP_GRID_DEFAULT = [0.5, 0.8, 1.0, 1.2, 1.5, 2.0, 3.0]
SL_GRID_DEFAULT = [1.5, 2.0, 3.0, 4.0, 5.0, 6.0]


def analyze_coin(coin: str, tf: str, tp_pct: float, sl_pct: float,
                 tp_grid: list, sl_grid: list, max_hold: int,
                 min_occ: int, max_combo: int, signal_spec: str = None):
    """Analyse complète d'un coin sur un timeframe."""

    print(f"\n{'='*60}")
    print(f"  ANALYSE : {coin} {tf}")
    print(f"{'='*60}")

    # 1. Chargement des données
    t0 = time.time()
    print(f"\n[1/6] Chargement des bougies 5m...", end=" ", flush=True)
    df_5m = load_candles(coin, "5m")
    print(f"{len(df_5m):,} bougies ({time.time()-t0:.1f}s)")

    if len(df_5m) < 1000:
        print(f"  ERREUR: pas assez de données ({len(df_5m)} bougies). Min: 1000.")
        return

    date_range = f"{df_5m['time'].iloc[0].strftime('%Y-%m-%d')} → {df_5m['time'].iloc[-1].strftime('%Y-%m-%d')}"
    print(f"  Période : {date_range}")

    # 2. Agrégation
    t0 = time.time()
    print(f"\n[2/6] Agrégation 5m → {tf}...", end=" ", flush=True)
    df = aggregate_tf(df_5m, tf)
    print(f"{len(df):,} bougies {tf} ({time.time()-t0:.1f}s)")

    if len(df) < 200:
        print(f"  ERREUR: pas assez de bougies après agrégation ({len(df)}). Min: 200.")
        return

    # 3. Indicateurs
    t0 = time.time()
    print(f"\n[3/6] Calcul des indicateurs...", end=" ", flush=True)
    df = compute_indicators(df)
    # Supprimer les lignes avec NaN dans les indicateurs principaux
    valid_from = df["sma_50"].first_valid_index()
    if valid_from is not None:
        df = df.loc[valid_from:].reset_index(drop=True)
    print(f"{len(df):,} bougies valides ({time.time()-t0:.1f}s)")

    # 4. Labellisation
    t0 = time.time()
    print(f"\n[4/6] Labellisation forward-looking (TP={tp_pct}% SL={sl_pct}% hold={max_hold})...",
          end=" ", flush=True)
    df_labeled = label_trades_vectorized(df, tp_pct, sl_pct, max_hold)

    long_wins = (df_labeled["long_result"] == "win").sum()
    long_losses = (df_labeled["long_result"] == "loss").sum()
    short_wins = (df_labeled["short_result"] == "win").sum()
    short_losses = (df_labeled["short_result"] == "loss").sum()
    print(f"({time.time()-t0:.1f}s)")
    print(f"  Baseline LONG  : {long_wins}W / {long_losses}L "
          f"({long_wins/(long_wins+long_losses)*100:.1f}% WR)" if long_wins + long_losses > 0 else "")
    print(f"  Baseline SHORT : {short_wins}W / {short_losses}L "
          f"({short_wins/(short_wins+short_losses)*100:.1f}% WR)" if short_wins + short_losses > 0 else "")

    # 5. Signaux
    t0 = time.time()
    if signal_spec:
        print(f"\n[5/6] Analyse du signal spécifique: {signal_spec}...", end=" ", flush=True)
        signal_names = parse_signal_spec(signal_spec)
        signals_df = compute_signals(df, signal_names)
        mask = signals_df.all(axis=1).values
        count = mask.sum()
        print(f"{count} occurrences ({time.time()-t0:.1f}s)")

        if count < 10:
            print(f"  Pas assez d'occurrences ({count}). Min: 10.")
            return

        combos = [(signal_spec, mask, int(count))]
    else:
        print(f"\n[5/6] Scan des signaux (1 à {max_combo} combinaisons, min {min_occ} occ.)...",
              end=" ", flush=True)
        signals_df = compute_signals(df)
        combos = scan_combinations(signals_df, max_combo=max_combo, min_occurrences=min_occ)
        print(f"{len(combos)} combinaisons trouvées ({time.time()-t0:.1f}s)")

    # 6. Statistiques
    t0 = time.time()
    print(f"\n[6/6] Calcul des statistiques...", end=" ", flush=True)

    long_results = []
    short_results = []

    for name, mask, count in combos:
        long_stats = compute_signal_stats(df_labeled, mask, "long")
        short_stats = compute_signal_stats(df_labeled, mask, "short")

        if long_stats:
            consistency = compute_consistency(df_labeled, mask, "long")
            long_stats.update(consistency)
            long_results.append((name, long_stats))

        if short_stats:
            consistency = compute_consistency(df_labeled, mask, "short")
            short_stats.update(consistency)
            short_results.append((name, short_stats))

    # Trier par EV décroissant
    long_results.sort(key=lambda x: x[1]["ev"], reverse=True)
    short_results.sort(key=lambda x: x[1]["ev"], reverse=True)

    print(f"({time.time()-t0:.1f}s)")

    # Affichage résumé console
    _print_top_results("LONG", long_results[:10])
    _print_top_results("SHORT", short_results[:10])

    # TP/SL scan pour les meilleurs signaux
    tp_sl_scans = {}
    top_for_scan = []
    # Prendre les N meilleurs LONG avec EV > 0
    for name, stats in long_results[:TOP_N_FOR_TP_SL_SCAN]:
        if stats["ev"] > 0:
            top_for_scan.append((name, "long"))
    # Et les N meilleurs SHORT
    for name, stats in short_results[:TOP_N_FOR_TP_SL_SCAN]:
        if stats["ev"] > 0:
            top_for_scan.append((name, "short"))

    if top_for_scan:
        print(f"\n  Scan TP/SL pour les {len(top_for_scan)} meilleurs signaux...")
        for signal_name, direction in top_for_scan:
            # Retrouver le mask
            mask = _find_mask(signal_name, combos)
            if mask is None:
                continue
            print(f"    {signal_name} ({direction})...", end=" ", flush=True)
            t1 = time.time()
            scan = find_optimal_tp_sl(df, mask, direction, tp_grid, sl_grid, max_hold)
            key = f"{signal_name} ({direction})"
            tp_sl_scans[key] = scan
            if scan:
                best = scan[0]
                print(f"optimal TP={best['tp']}% SL={best['sl']}% "
                      f"(EV={best['ev']:.3f}%, WR={best['win_rate']:.1f}%) "
                      f"({time.time()-t1:.1f}s)")
            else:
                print(f"pas de résultat ({time.time()-t1:.1f}s)")

    # Génération du rapport
    filepath = generate_report(
        coin=coin,
        tf=tf,
        long_results=long_results,
        short_results=short_results,
        tp_sl_scans=tp_sl_scans,
        n_candles=len(df),
        date_range=date_range,
    )
    print(f"\n  Rapport généré : {filepath}")


def _find_mask(signal_name: str, combos: list):
    """Retrouve le mask d'un signal dans la liste des combos."""
    for name, mask, count in combos:
        if name == signal_name:
            return mask
    return None


def _print_top_results(direction: str, results: list):
    """Affiche les top résultats dans la console."""
    if not results:
        return

    print(f"\n  Top 10 {direction}:")
    print(f"  {'Signal':<50} {'Count':>6} {'WR%':>6} {'EV%':>8} {'PF':>6} {'Total%':>8}")
    print(f"  {'-'*50} {'-'*6} {'-'*6} {'-'*8} {'-'*6} {'-'*8}")

    for name, s in results[:10]:
        pf_str = f"{s['profit_factor']:.2f}" if s['profit_factor'] < 100 else "∞"
        print(f"  {name:<50} {s['count']:>6} {s['win_rate']:>5.1f}% {s['ev']:>7.3f}% "
              f"{pf_str:>6} {s['total_pnl']:>7.1f}%")


def main():
    parser = argparse.ArgumentParser(
        description="Analyse probabiliste des signaux de trading",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Exemples:
  python3 tools/strategy_analyzer.py --coin ETH --tf 1h
  python3 tools/strategy_analyzer.py --coin ETH,SOL --tf 1h
  python3 tools/strategy_analyzer.py --coin ETH --tf 1h --signal "ema_uptrend+rsi_os_40+stoch_os_30"
  python3 tools/strategy_analyzer.py --coin SOL --tf 1h --tp 0.8,1.0,1.2,1.5 --sl 3.0,4.0,5.0
        """,
    )

    parser.add_argument("--coin", default=None, help="Coin(s) à analyser (ex: ETH ou ETH,SOL)")
    parser.add_argument("--tf", default="1h", help="Timeframe (5m, 15m, 1h, 4h, 1d). Défaut: 1h")
    parser.add_argument("--tp", type=str, default=None,
                        help=f"Grille TP en %%, séparés par virgules. Défaut: {','.join(str(x) for x in TP_GRID_DEFAULT)}")
    parser.add_argument("--sl", type=str, default=None,
                        help=f"Grille SL en %%, séparés par virgules. Défaut: {','.join(str(x) for x in SL_GRID_DEFAULT)}")
    parser.add_argument("--signal", type=str, default=None,
                        help="Signal spécifique à analyser (ex: ema_uptrend+rsi_os_40+stoch_os_30)")
    parser.add_argument("--max-hold", type=int, default=DEFAULT_MAX_HOLD,
                        help=f"Max holding period en bougies. Défaut: {DEFAULT_MAX_HOLD}")
    parser.add_argument("--min-occ", type=int, default=DEFAULT_MIN_OCC,
                        help=f"Min occurrences pour significativité. Défaut: {DEFAULT_MIN_OCC}")
    parser.add_argument("--max-combo", type=int, default=DEFAULT_MAX_COMBO,
                        help=f"Max signaux combinés (1-3). Défaut: {DEFAULT_MAX_COMBO}")
    parser.add_argument("--base-tp", type=float, default=DEFAULT_TP,
                        help=f"TP de base pour le scan initial en %%. Défaut: {DEFAULT_TP}")
    parser.add_argument("--base-sl", type=float, default=DEFAULT_SL,
                        help=f"SL de base pour le scan initial en %%. Défaut: {DEFAULT_SL}")
    parser.add_argument("--list-signals", action="store_true",
                        help="Affiche la liste des signaux disponibles")
    parser.add_argument("--list-coins", action="store_true",
                        help="Affiche les coins disponibles dans la DB")

    args = parser.parse_args()

    if args.list_signals:
        print("Signaux disponibles:")
        for name in sorted(SIGNAL_DEFS.keys()):
            print(f"  {name}")
        return

    if args.list_coins:
        print("Coins disponibles:")
        for coin in available_coins():
            count = candle_count(coin, "5m")
            print(f"  {coin}: {count:,} bougies 5m")
        return

    if args.coin is None:
        parser.error("--coin est requis (ex: --coin ETH ou --coin ETH,SOL)")

    # Parse grilles TP/SL
    tp_grid = [float(x) for x in args.tp.split(",")] if args.tp else TP_GRID_DEFAULT
    sl_grid = [float(x) for x in args.sl.split(",")] if args.sl else SL_GRID_DEFAULT

    coins = [c.strip().upper() for c in args.coin.split(",")]

    print(f"Strategy Analyzer — Analyse Probabiliste")
    print(f"Config: TP base={args.base_tp}% SL base={args.base_sl}% "
          f"max_hold={args.max_hold} min_occ={args.min_occ} max_combo={args.max_combo}")
    print(f"Grille TP: {tp_grid}")
    print(f"Grille SL: {sl_grid}")

    t_start = time.time()

    for coin in coins:
        analyze_coin(
            coin=coin,
            tf=args.tf,
            tp_pct=args.base_tp,
            sl_pct=args.base_sl,
            tp_grid=tp_grid,
            sl_grid=sl_grid,
            max_hold=args.max_hold,
            min_occ=args.min_occ,
            max_combo=args.max_combo,
            signal_spec=args.signal,
        )

    elapsed = time.time() - t_start
    print(f"\n{'='*60}")
    print(f"  Terminé en {elapsed:.1f}s")
    print(f"{'='*60}")


if __name__ == "__main__":
    main()
