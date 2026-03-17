# Analyse Probabiliste — ETH 4h

*Généré le 2026-03-16 13:54*

- **Coin** : ETH
- **Timeframe** : 4h
- **Bougies analysées** : 17,793
- **Période** : 2017-08-17 → 2026-03-16
- **Fees** : 0.06% round-trip (maker entry + taker exit)

## Top 20 Signaux LONG (par Expected Value)

| # | Signal | Count | WR% | EV% | PF | Total PnL% | CI 95% | Sharpe |
|---|--------|-------|-----|-----|----|------------|--------|--------|
| 1 | `rsi_os_35+adx_ranging+sma20_below_sma50` | 79 | 53.2 | -1.295 | 0.32 | -102.3 | [42.3-63.8] | -0.499 |

## Top 20 Signaux SHORT (par Expected Value)

| # | Signal | Count | WR% | EV% | PF | Total PnL% | CI 95% | Sharpe |
|---|--------|-------|-----|-----|----|------------|--------|--------|
| 1 | `rsi_os_35+adx_ranging+sma20_below_sma50` | 79 | 89.9 | 0.715 | 4.98 | 56.5 | [81.3-94.8] | 0.544 |

## Top 10 Signaux par Win Rate (min 50 occurrences)

### LONG

| # | Signal | Count | WR% | EV% | PF | Total PnL% | CI 95% | Sharpe |
|---|--------|-------|-----|-----|----|------------|--------|--------|
| 1 | `rsi_os_35+adx_ranging+sma20_below_sma50` | 79 | 53.2 | -1.295 | 0.32 | -102.3 | [42.3-63.8] | -0.499 |

### SHORT

| # | Signal | Count | WR% | EV% | PF | Total PnL% | CI 95% | Sharpe |
|---|--------|-------|-----|-----|----|------------|--------|--------|
| 1 | `rsi_os_35+adx_ranging+sma20_below_sma50` | 79 | 89.9 | 0.715 | 4.98 | 56.5 | [81.3-94.8] | 0.544 |

## Matrices TP/SL (meilleurs signaux)

### rsi_os_35+adx_ranging+sma20_below_sma50 (short)

| TP \ SL | 1.5% | 2.0% | 3.0% | 4.0% | 5.0% | 6.0% |
|---------|------|------|------|------|------|------|
| **0.5%** | 0.237 | 0.250 | 0.174 | 0.286 | 0.260 | 0.400 |
| **0.8%** | 0.303 | 0.364 | 0.285 | 0.392 | 0.342 | 0.377 |
| **1.0%** | 0.307 | 0.498 | 0.409 | 0.575 | 0.524 | 0.562 |
| **1.2%** | 0.354 | 0.586 | 0.475 | 0.715 | 0.664 | 0.614 |
| **1.5%** | 0.453 | 0.700 | 0.555 | 0.923 | 0.873 | 0.822 |
| **2.0%** | 0.567 | 0.789 | 0.699 | 1.156 | 1.093 | 1.029 |
| **3.0%** | 1.003 | 1.098 | 1.032 | 1.562 | **1.765** | 1.702 |

**Optimal** : TP 3.0% / SL 5.0% (EV=1.765%, WR=77.2%, PF=7.09, R:R=1:0.6)

## Recommandations

### Meilleures combinaisons SHORT à implémenter

1. **`rsi_os_35+adx_ranging+sma20_below_sma50`**
   - Conditions : rsi_os_35 AND adx_ranging AND sma20_below_sma50
   - Win rate : 89.9% (79 trades)
   - EV : 0.715% par trade
   - Profit Factor : 4.98
   - IC 95% WR : [81.3% - 94.8%]
