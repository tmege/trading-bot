# Analyse Probabiliste — SOL 4h

*Généré le 2026-03-16 13:54*

- **Coin** : SOL
- **Timeframe** : 4h
- **Bougies analysées** : 11,543
- **Période** : 2020-08-11 → 2026-03-16
- **Fees** : 0.06% round-trip (maker entry + taker exit)

## Top 20 Signaux LONG (par Expected Value)

| # | Signal | Count | WR% | EV% | PF | Total PnL% | CI 95% | Sharpe |
|---|--------|-------|-----|-----|----|------------|--------|--------|
| 1 | `rsi_ob_55+below_bb_mid+obv_below_sma` | 33 | 78.8 | 0.037 | 1.04 | 1.2 | [62.2-89.3] | 0.017 |

## Top 20 Signaux SHORT (par Expected Value)

| # | Signal | Count | WR% | EV% | PF | Total PnL% | CI 95% | Sharpe |
|---|--------|-------|-----|-----|----|------------|--------|--------|
| 1 | `rsi_ob_55+below_bb_mid+obv_below_sma` | 33 | 60.6 | -0.908 | 0.43 | -30.0 | [43.7-75.3] | -0.358 |

## Top 10 Signaux par Win Rate (min 50 occurrences)

## Matrices TP/SL (meilleurs signaux)

### rsi_ob_55+below_bb_mid+obv_below_sma (long)

| TP \ SL | 1.5% | 2.0% | 3.0% | 4.0% | 5.0% | 6.0% |
|---------|------|------|------|------|------|------|
| **0.5%** | 0.076 | -0.015 | -0.090 | -0.105 | -0.227 | -0.151 |
| **0.8%** | 0.182 | 0.061 | 0.049 | 0.013 | 0.037 | 0.122 |
| **1.0%** | 0.334 | 0.213 | 0.213 | 0.182 | 0.213 | 0.304 |
| **1.2%** | 0.322 | 0.170 | 0.122 | 0.037 | 0.013 | 0.049 |
| **1.5%** | 0.349 | 0.167 | 0.213 | 0.107 | 0.061 | -0.151 |
| **2.0%** | 0.455 | 0.364 | 0.425 | 0.304 | 0.455 | 0.243 |
| **3.0%** | 0.894 | 0.667 | **1.122** | 0.819 | 1.001 | 0.758 |

**Optimal** : TP 3.0% / SL 3.0% (EV=1.122%, WR=69.7%, PF=2.21, R:R=1:1.0)

## Recommandations

### Meilleures combinaisons LONG à implémenter

1. **`rsi_ob_55+below_bb_mid+obv_below_sma`**
   - Conditions : rsi_ob_55 AND below_bb_mid AND obv_below_sma
   - Win rate : 78.8% (33 trades)
   - EV : 0.037% par trade
   - Profit Factor : 1.04
   - IC 95% WR : [62.2% - 89.3%]
