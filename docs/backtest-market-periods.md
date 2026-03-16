# Rapport Backtest Multi-Timeframe — Periodes de Marche

> **Date**: 2026-03-16 | **Simulation**: bougies 5m | **Balance initiale**: $100 | **Levier max**: 5x
> **Strategies**: regime_adaptive_1h, squeeze_breakout_1h, ichimoku_trend_4h
> **Coins**: BTC, ETH, SOL, DOGE | **Resultats**: 14/84 valides

## 1. Vue d'ensemble par periode de marche

| Periode | Strat | Coin | Return | Trades | Win% | PF | Sharpe | Max DD | Alpha vs B&H |
|---------|-------|------|--------|--------|------|-------|--------|--------|--------------|
| Bull Run 2020-21 | regime_adaptive_1h | ETH | +11.7% | 264 | 58.7 | 1.91 | 4.16 | -5.6% | -445.9% |
|  | regime_adaptive_1h | SOL | -1.4% | 120 | 47.5 | 1.02 | 0.10 | -4.1% | -3025.7% |
| | | | | | | | | | |
| Bear Market 2022 | regime_adaptive_1h | ETH | +3.9% | 302 | 53.6 | 1.54 | 3.36 | -1.9% | +334.9% |
|  | regime_adaptive_1h | SOL | +5.7% | 312 | 55.1 | 1.54 | 3.31 | -2.7% | +337.3% |
| | | | | | | | | | |
| High Vol 2022 | regime_adaptive_1h | ETH | +9.3% | 342 | 56.7 | 1.81 | 4.46 | -1.5% | +160.2% |
|  | regime_adaptive_1h | SOL | +10.1% | 362 | 56.1 | 1.73 | 3.63 | -7.2% | +376.3% |
| | | | | | | | | | |
| Low Vol 2023 | regime_adaptive_1h | ETH | -0.3% | 4 | 0.0 | 0.00 | -2.66 | -0.3% | +47.2% |
|  | regime_adaptive_1h | SOL | +1.8% | 202 | 54.5 | 1.50 | 3.46 | -1.8% | +89.6% |
| | | | | | | | | | |
| Bull ETF 2024 | regime_adaptive_1h | ETH | -0.1% | 77 | 49.4 | 1.24 | 0.97 | -1.0% | -395.1% |
|  | regime_adaptive_1h | SOL | +2.2% | 213 | 51.2 | 1.38 | 2.39 | -2.0% | -371.2% |
| | | | | | | | | | |
| Bull ATH 2024-25 | regime_adaptive_1h | ETH | -4.3% | 124 | 44.4 | 0.68 | -2.43 | -4.3% | -7.9% |
|  | regime_adaptive_1h | SOL | +5.1% | 188 | 58.0 | 1.83 | 4.39 | -1.6% | -19.2% |
| | | | | | | | | | |
| Recent 6M | regime_adaptive_1h | ETH | +3.9% | 219 | 59.4 | 1.70 | 3.62 | -2.0% | +138.0% |
|  | regime_adaptive_1h | SOL | +2.5% | 236 | 52.5 | 1.51 | 3.07 | -2.1% | +153.6% |
| | | | | | | | | | |

## 2. Performance moyenne par strategie

| Strategie | Return moy | Win% moy | PF moy | Sharpe moy | Max DD moy | Trades moy | Alpha moy |
|-----------|-----------|----------|--------|------------|------------|------------|-----------|
| regime_adaptive_1h | +3.6% | 49.8% | 1.39 | 2.27 | 2.7% | 212 | -187.7% |

## 3. Performance moyenne par coin

| Coin | Return moy | Win% moy | PF moy | Sharpe moy | Max DD moy | Trades moy |
|------|-----------|----------|--------|------------|------------|------------|
| ETH | +3.4% | 46.0% | 1.27 | 1.64 | 2.4% | 190 |
| SOL | +3.7% | 53.6% | 1.50 | 2.91 | 3.1% | 233 |

## 4. Performance par type de marche

| Type marche | Return moy | Win% moy | PF moy | Sharpe moy | Max DD moy | Alpha moy |
|-------------|-----------|----------|--------|------------|------------|-----------|
| **Bull** | +2.2% | 51.5% | 1.34 | 1.60 | 3.1% | -710.8% |
| **Bear** | +4.8% | 54.4% | 1.54 | 3.34 | 2.3% | +336.1% |
| **High Vol** | +9.7% | 56.4% | 1.77 | 4.05 | 4.4% | +268.3% |
| **Low Vol** | +0.8% | 27.2% | 0.75 | 0.40 | 1.0% | +68.4% |
| **Recent** | +3.2% | 56.0% | 1.60 | 3.35 | 2.1% | +145.8% |

## 5. Walk-forward IS vs OOS (overfitting check)

| Strategie | Coin | Return IS moy | Return OOS moy | Sharpe IS | Sharpe OOS | Decay | Overfit? |
|-----------|------|--------------|----------------|-----------|------------|-------|----------|
| regime_adaptive_1h | ETH | +1.9% | +1.4% | 1.19 | 2.52 | +111% | NON |
| regime_adaptive_1h | SOL | +1.1% | +2.6% | 2.29 | 3.80 | +66% | NON |

## 6. Top 10 meilleures performances

| # | Periode | Strat | Coin | Return | Trades | Win% | PF | Sharpe | Verdict |
|---|---------|-------|------|--------|--------|------|-----|--------|---------|
| 1 | Bull Run 2020-21 | regime_adaptive_1h | ETH | +11.7% | 264 | 58.7 | 1.91 | 4.16 | MARGINAL |
| 2 | High Vol 2022 | regime_adaptive_1h | SOL | +10.1% | 362 | 56.1 | 1.73 | 3.63 | DEPLOYABLE |
| 3 | High Vol 2022 | regime_adaptive_1h | ETH | +9.3% | 342 | 56.7 | 1.81 | 4.46 | DEPLOYABLE |
| 4 | Bear Market 2022 | regime_adaptive_1h | SOL | +5.7% | 312 | 55.1 | 1.54 | 3.31 | DEPLOYABLE |
| 5 | Bull ATH 2024-25 | regime_adaptive_1h | SOL | +5.1% | 188 | 58.0 | 1.83 | 4.39 | MARGINAL |
| 6 | Recent 6M | regime_adaptive_1h | ETH | +3.9% | 219 | 59.4 | 1.70 | 3.62 | DEPLOYABLE |
| 7 | Bear Market 2022 | regime_adaptive_1h | ETH | +3.9% | 302 | 53.6 | 1.54 | 3.36 | DEPLOYABLE |
| 8 | Recent 6M | regime_adaptive_1h | SOL | +2.5% | 236 | 52.5 | 1.51 | 3.07 | DEPLOYABLE |
| 9 | Bull ETF 2024 | regime_adaptive_1h | SOL | +2.2% | 213 | 51.2 | 1.38 | 2.39 | MARGINAL |
| 10 | Low Vol 2023 | regime_adaptive_1h | SOL | +1.8% | 202 | 54.5 | 1.50 | 3.46 | DEPLOYABLE |

## 7. Top 10 pires performances

| # | Periode | Strat | Coin | Return | Trades | Win% | PF | Sharpe | Max DD | Verdict |
|---|---------|-------|------|--------|--------|------|-----|--------|--------|---------|
| 1 | Bull ATH 2024-25 | regime_adaptive_1h | SOL | +5.1% | 188 | 58.0 | 1.83 | 4.39 | -1.6% | MARGINAL |
| 2 | Recent 6M | regime_adaptive_1h | ETH | +3.9% | 219 | 59.4 | 1.70 | 3.62 | -2.0% | DEPLOYABLE |
| 3 | Bear Market 2022 | regime_adaptive_1h | ETH | +3.9% | 302 | 53.6 | 1.54 | 3.36 | -1.9% | DEPLOYABLE |
| 4 | Recent 6M | regime_adaptive_1h | SOL | +2.5% | 236 | 52.5 | 1.51 | 3.07 | -2.1% | DEPLOYABLE |
| 5 | Bull ETF 2024 | regime_adaptive_1h | SOL | +2.2% | 213 | 51.2 | 1.38 | 2.39 | -2.0% | MARGINAL |
| 6 | Low Vol 2023 | regime_adaptive_1h | SOL | +1.8% | 202 | 54.5 | 1.50 | 3.46 | -1.8% | DEPLOYABLE |
| 7 | Bull ETF 2024 | regime_adaptive_1h | ETH | -0.1% | 77 | 49.4 | 1.24 | 0.97 | -1.0% | MARGINAL |
| 8 | Low Vol 2023 | regime_adaptive_1h | ETH | -0.3% | 4 | 0.0 | 0.00 | -2.66 | -0.3% | INSUFFISANT |
| 9 | Bull Run 2020-21 | regime_adaptive_1h | SOL | -1.4% | 120 | 47.5 | 1.02 | 0.10 | -4.1% | MARGINAL |
| 10 | Bull ATH 2024-25 | regime_adaptive_1h | ETH | -4.3% | 124 | 44.4 | 0.68 | -2.43 | -4.3% | ABANDON |

## 8. Distribution des verdicts OOS

| Verdict | Count | % |
|---------|-------|---|
| DEPLOYABLE | 7 | 50% |
| MARGINAL | 5 | 36% |
| INSUFFISANT | 1 | 7% |
| ABANDON | 1 | 7% |

| Strategie | DEPLOYABLE | A_OPTIMISER | MARGINAL | ABANDON | INSUFFISANT |
|-----------|------------|-------------|----------|---------|-------------|
| regime_adaptive_1h | 7 | 0 | 5 | 1 | 1 |

---

*Genere automatiquement le 2026-03-16. Simulation 5m multi-timeframe, walk-forward IS/OOS 60/40.*
