#!/usr/bin/env python3
"""Parse backtest JSON results and output a comprehensive report."""

import json
import os
import sys
from pathlib import Path

DIR = Path("./data/backtest_results/market_periods")

PERIODS = {
    "bull_2020_21": "Bull Run 2020-21",
    "bear_2022": "Bear Market 2022",
    "highvol_2022": "High Vol 2022",
    "lowvol_2023": "Low Vol 2023",
    "bull_etf_2024": "Bull ETF 2024",
    "bull_ath_2024": "Bull ATH 2024-25",
    "recent": "Recent 6M",
}

STRATEGIES = ["regime_adaptive_1h"]
COINS = ["ETH", "SOL"]

def load_result(period, strat, coin):
    f = DIR / f"{period}_{strat}_{coin}.json"
    if not f.exists():
        return None
    try:
        with open(f) as fh:
            return json.load(fh)
    except:
        return None

def fmt_pct(v):
    if v is None: return "N/A"
    return f"{v:+.1f}%"

def fmt_num(v, decimals=2):
    if v is None: return "N/A"
    return f"{v:.{decimals}f}"

# Collect all data
all_data = {}
for period in PERIODS:
    for strat in STRATEGIES:
        for coin in COINS:
            r = load_result(period, strat, coin)
            if r:
                all_data[(period, strat, coin)] = r

print("# Rapport Backtest Multi-Timeframe — Periodes de Marche")
print()
print(f"> **Date**: 2026-03-16 | **Simulation**: bougies 5m | **Balance initiale**: $100 | **Levier max**: 5x")
print(f"> **Strategies**: regime_adaptive_1h, squeeze_breakout_1h, ichimoku_trend_4h")
print(f"> **Coins**: BTC, ETH, SOL, DOGE | **Resultats**: {len(all_data)}/84 valides")
print()

# ══════════════════════════════════════════════════════════════
# TABLE 1: Vue d'ensemble par periode
# ══════════════════════════════════════════════════════════════
print("## 1. Vue d'ensemble par periode de marche")
print()
print("| Periode | Strat | Coin | Return | Trades | Win% | PF | Sharpe | Max DD | Alpha vs B&H |")
print("|---------|-------|------|--------|--------|------|-------|--------|--------|--------------|")

for period, period_label in PERIODS.items():
    first = True
    for strat in STRATEGIES:
        for coin in COINS:
            key = (period, strat, coin)
            if key not in all_data:
                continue
            d = all_data[key]
            s = d.get("stats", {})
            so = d.get("stats_oos", {})
            alpha = d.get("alpha", 0)

            plabel = period_label if first else ""
            first = False

            print(f"| {plabel} | {strat} | {coin} | "
                  f"{fmt_pct(s.get('return_pct'))} | "
                  f"{s.get('total_trades', 0)} | "
                  f"{fmt_num(s.get('win_rate'), 1)} | "
                  f"{fmt_num(s.get('profit_factor'), 2)} | "
                  f"{fmt_num(s.get('sharpe_ratio'), 2)} | "
                  f"{fmt_pct(-s.get('max_drawdown_pct', 0))} | "
                  f"{fmt_pct(alpha)} |")
    print(f"| | | | | | | | | | |")

# ══════════════════════════════════════════════════════════════
# TABLE 2: Comparaison strategies (moyennes par strat)
# ══════════════════════════════════════════════════════════════
print()
print("## 2. Performance moyenne par strategie")
print()
print("| Strategie | Return moy | Win% moy | PF moy | Sharpe moy | Max DD moy | Trades moy | Alpha moy |")
print("|-----------|-----------|----------|--------|------------|------------|------------|-----------|")

for strat in STRATEGIES:
    returns, winrates, pfs, sharpes, dds, trades_list, alphas = [], [], [], [], [], [], []
    for period in PERIODS:
        for coin in COINS:
            key = (period, strat, coin)
            if key not in all_data:
                continue
            s = all_data[key].get("stats", {})
            alpha = all_data[key].get("alpha", 0)
            returns.append(s.get("return_pct", 0))
            winrates.append(s.get("win_rate", 0))
            pf = s.get("profit_factor", 0)
            if pf > 100: pf = 100  # cap inf PF
            pfs.append(pf)
            sharpes.append(s.get("sharpe_ratio", 0))
            dds.append(s.get("max_drawdown_pct", 0))
            trades_list.append(s.get("total_trades", 0))
            alphas.append(alpha)

    n = len(returns)
    if n == 0:
        continue

    print(f"| {strat} | "
          f"{sum(returns)/n:+.1f}% | "
          f"{sum(winrates)/n:.1f}% | "
          f"{sum(pfs)/n:.2f} | "
          f"{sum(sharpes)/n:.2f} | "
          f"{sum(dds)/n:.1f}% | "
          f"{sum(trades_list)/n:.0f} | "
          f"{sum(alphas)/n:+.1f}% |")

# ══════════════════════════════════════════════════════════════
# TABLE 3: Comparaison coins
# ══════════════════════════════════════════════════════════════
print()
print("## 3. Performance moyenne par coin")
print()
print("| Coin | Return moy | Win% moy | PF moy | Sharpe moy | Max DD moy | Trades moy |")
print("|------|-----------|----------|--------|------------|------------|------------|")

for coin in COINS:
    returns, winrates, pfs, sharpes, dds, trades_list = [], [], [], [], [], []
    for period in PERIODS:
        for strat in STRATEGIES:
            key = (period, strat, coin)
            if key not in all_data:
                continue
            s = all_data[key].get("stats", {})
            returns.append(s.get("return_pct", 0))
            winrates.append(s.get("win_rate", 0))
            pf = s.get("profit_factor", 0)
            if pf > 100: pf = 100
            pfs.append(pf)
            sharpes.append(s.get("sharpe_ratio", 0))
            dds.append(s.get("max_drawdown_pct", 0))
            trades_list.append(s.get("total_trades", 0))

    n = len(returns)
    if n == 0:
        continue

    print(f"| {coin} | "
          f"{sum(returns)/n:+.1f}% | "
          f"{sum(winrates)/n:.1f}% | "
          f"{sum(pfs)/n:.2f} | "
          f"{sum(sharpes)/n:.2f} | "
          f"{sum(dds)/n:.1f}% | "
          f"{sum(trades_list)/n:.0f} |")

# ══════════════════════════════════════════════════════════════
# TABLE 4: Performance par type de marche (agrege)
# ══════════════════════════════════════════════════════════════
print()
print("## 4. Performance par type de marche")
print()

market_types = {
    "Bull": ["bull_2020_21", "bull_etf_2024", "bull_ath_2024"],
    "Bear": ["bear_2022"],
    "High Vol": ["highvol_2022"],
    "Low Vol": ["lowvol_2023"],
    "Recent": ["recent"],
}

print("| Type marche | Return moy | Win% moy | PF moy | Sharpe moy | Max DD moy | Alpha moy |")
print("|-------------|-----------|----------|--------|------------|------------|-----------|")

for mtype, periods_list in market_types.items():
    returns, winrates, pfs, sharpes, dds, alphas = [], [], [], [], [], []
    for period in periods_list:
        for strat in STRATEGIES:
            for coin in COINS:
                key = (period, strat, coin)
                if key not in all_data:
                    continue
                s = all_data[key].get("stats", {})
                alpha = all_data[key].get("alpha", 0)
                returns.append(s.get("return_pct", 0))
                winrates.append(s.get("win_rate", 0))
                pf = s.get("profit_factor", 0)
                if pf > 100: pf = 100
                pfs.append(pf)
                sharpes.append(s.get("sharpe_ratio", 0))
                dds.append(s.get("max_drawdown_pct", 0))
                alphas.append(alpha)

    n = len(returns)
    if n == 0:
        continue

    print(f"| **{mtype}** | "
          f"{sum(returns)/n:+.1f}% | "
          f"{sum(winrates)/n:.1f}% | "
          f"{sum(pfs)/n:.2f} | "
          f"{sum(sharpes)/n:.2f} | "
          f"{sum(dds)/n:.1f}% | "
          f"{sum(alphas)/n:+.1f}% |")

# ══════════════════════════════════════════════════════════════
# TABLE 5: Walk-forward (IS vs OOS)
# ══════════════════════════════════════════════════════════════
print()
print("## 5. Walk-forward IS vs OOS (overfitting check)")
print()
print("| Strategie | Coin | Return IS moy | Return OOS moy | Sharpe IS | Sharpe OOS | Decay | Overfit? |")
print("|-----------|------|--------------|----------------|-----------|------------|-------|----------|")

for strat in STRATEGIES:
    for coin in COINS:
        is_rets, oos_rets, is_sharpes, oos_sharpes = [], [], [], []
        for period in PERIODS:
            key = (period, strat, coin)
            if key not in all_data:
                continue
            si = all_data[key].get("stats_is", {})
            so = all_data[key].get("stats_oos", {})
            is_rets.append(si.get("return_pct", 0))
            oos_rets.append(so.get("return_pct", 0))
            is_sharpes.append(si.get("sharpe_ratio", 0))
            oos_sharpes.append(so.get("sharpe_ratio", 0))

        n = len(is_rets)
        if n == 0:
            continue

        is_r = sum(is_rets)/n
        oos_r = sum(oos_rets)/n
        is_s = sum(is_sharpes)/n
        oos_s = sum(oos_sharpes)/n
        decay = ((oos_s - is_s) / is_s * 100) if is_s != 0 else 0
        overfit = "OUI" if decay < -50 else "NON"

        print(f"| {strat} | {coin} | "
              f"{is_r:+.1f}% | {oos_r:+.1f}% | "
              f"{is_s:.2f} | {oos_s:.2f} | "
              f"{decay:+.0f}% | {overfit} |")

# ══════════════════════════════════════════════════════════════
# TABLE 6: Meilleures et pires performances
# ══════════════════════════════════════════════════════════════
print()
print("## 6. Top 10 meilleures performances")
print()
print("| # | Periode | Strat | Coin | Return | Trades | Win% | PF | Sharpe | Verdict |")
print("|---|---------|-------|------|--------|--------|------|-----|--------|---------|")

ranked = sorted(all_data.items(), key=lambda x: x[1].get("stats", {}).get("return_pct", 0), reverse=True)
for i, (key, d) in enumerate(ranked[:10]):
    period, strat, coin = key
    s = d.get("stats", {})
    v = d.get("verdict", "?")
    print(f"| {i+1} | {PERIODS[period]} | {strat} | {coin} | "
          f"{fmt_pct(s.get('return_pct'))} | "
          f"{s.get('total_trades', 0)} | "
          f"{fmt_num(s.get('win_rate'), 1)} | "
          f"{fmt_num(s.get('profit_factor'), 2)} | "
          f"{fmt_num(s.get('sharpe_ratio'), 2)} | "
          f"{v} |")

print()
print("## 7. Top 10 pires performances")
print()
print("| # | Periode | Strat | Coin | Return | Trades | Win% | PF | Sharpe | Max DD | Verdict |")
print("|---|---------|-------|------|--------|--------|------|-----|--------|--------|---------|")

for i, (key, d) in enumerate(ranked[-10:]):
    period, strat, coin = key
    s = d.get("stats", {})
    v = d.get("verdict", "?")
    print(f"| {i+1} | {PERIODS[period]} | {strat} | {coin} | "
          f"{fmt_pct(s.get('return_pct'))} | "
          f"{s.get('total_trades', 0)} | "
          f"{fmt_num(s.get('win_rate'), 1)} | "
          f"{fmt_num(s.get('profit_factor'), 2)} | "
          f"{fmt_num(s.get('sharpe_ratio'), 2)} | "
          f"{fmt_pct(-s.get('max_drawdown_pct', 0))} | "
          f"{v} |")

# ══════════════════════════════════════════════════════════════
# TABLE 7: Verdicts
# ══════════════════════════════════════════════════════════════
print()
print("## 8. Distribution des verdicts OOS")
print()

verdicts = {}
for key, d in all_data.items():
    v = d.get("verdict", "UNKNOWN")
    verdicts[v] = verdicts.get(v, 0) + 1

print("| Verdict | Count | % |")
print("|---------|-------|---|")
total_v = sum(verdicts.values())
for v in sorted(verdicts, key=verdicts.get, reverse=True):
    pct = verdicts[v] / total_v * 100
    print(f"| {v} | {verdicts[v]} | {pct:.0f}% |")

# Verdict par strategie
print()
print("| Strategie | DEPLOYABLE | A_OPTIMISER | MARGINAL | ABANDON | INSUFFISANT |")
print("|-----------|------------|-------------|----------|---------|-------------|")
for strat in STRATEGIES:
    sv = {}
    for key, d in all_data.items():
        if key[1] == strat:
            v = d.get("verdict", "?")
            sv[v] = sv.get(v, 0) + 1
    total_s = sum(sv.values())
    print(f"| {strat} | "
          f"{sv.get('DEPLOYABLE', 0)} | "
          f"{sv.get('A_OPTIMISER', 0)} | "
          f"{sv.get('MARGINAL', 0)} | "
          f"{sv.get('ABANDON', 0)} | "
          f"{sv.get('INSUFFISANT', 0)} |")

print()
print("---")
print()
print("*Genere automatiquement le 2026-03-16. Simulation 5m multi-timeframe, walk-forward IS/OOS 60/40.*")
