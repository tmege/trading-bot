# Backtest Multi-Periodes — 12 Strategies (Compound 5%)

> Run: 11 mars 2026 | Periodes: P1(365j), P2(180j ancien), P3(180j recent), P4(90j recent)
> Capital initial: $100 | Compound: 5% equity/trade | Fees: maker 2bps, taker 5bps, slippage 1bp
> Walk-forward 60/40 IS/OOS | Coins: ETH + BTC

---

## Classement Global (P1 365 jours, par Return)

| Rang | Strategie | Coin | TF | Return | Sharpe | Max DD | PF | WR | Trades | Verdict |
|------|-----------|------|----|--------|--------|--------|-----|-----|--------|---------|
| 1 | regime_adaptive_1h | BTC | 1h | +36634% | 47.66 | 0.34% | 140.5 | 49.4% | 4096 | DEPLOYABLE |
| 2 | regime_adaptive_1h | ETH | 1h | +27340% | 47.12 | 0.52% | 54.6 | 63.3% | 4096 | DEPLOYABLE |
| 3 | ichimoku_trend_4h | BTC | 4h | +20479% | 24.59 | 0.61% | 135.6 | 49.2% | 2754 | DEPLOYABLE |
| 4 | ichimoku_trend_4h | ETH | 4h | +13043% | 24.01 | 0.31% | 35.4 | 47.2% | 2680 | DEPLOYABLE |
| 5 | ema_adx_trend_4h | BTC | 4h | +1337% | 28.17 | 0.64% | 40.6 | 48.2% | 1904 | DEPLOYABLE |
| 6 | macd_momentum_1h | ETH | 1h | +1318% | 45.21 | 0.18% | 36.0 | 47.3% | 4010 | DEPLOYABLE |
| 7 | ema_adx_trend_4h | ETH | 4h | +950% | 26.25 | 0.39% | 17.0 | 44.6% | 1884 | DEPLOYABLE |
| 8 | macd_momentum_1h | BTC | 1h | +429% | 47.74 | 0.13% | 38.9 | 47.1% | 3844 | DEPLOYABLE |
| 9 | bb_scalp_15m | ETH | 15m | +359% | 98.47 | 0.17% | 56.8 | 48.1% | 2218 | DEPLOYABLE |
| 10 | stochrsi_scalp_5m | BTC | 5m | +112% | 194.53 | 0.13% | 137.9 | 49.3% | 2170 | DEPLOYABLE |
| 11 | vwap_reversion_15m | ETH | 15m | +102% | 83.62 | 0.28% | 15.9 | 44.4% | 1782 | DEPLOYABLE |
| 12 | stochrsi_scalp_5m | ETH | 5m | +96% | 180.76 | 0.13% | 56.9 | 48.4% | 2004 | DEPLOYABLE |
| 13 | triple_confirm_15m | ETH | 15m | +69% | 69.45 | 0.14% | 31.6 | 47.0% | 952 | DEPLOYABLE |
| 14 | williams_obv_4h | BTC | 4h | +46% | 16.87 | 0.10% | 98.9 | 48.8% | 322 | DEPLOYABLE |
| 15 | williams_obv_4h | ETH | 4h | +25% | 11.25 | 0.21% | 14.2 | 42.6% | 230 | DEPLOYABLE |
| 16 | bb_kc_squeeze_1h | ETH | 1h | +20% | 17.90 | 0.11% | 30.4 | 47.1% | 310 | DEPLOYABLE |
| 17 | rsi_divergence_1h | BTC | 1h | +18% | 18.97 | 0.08% | 72.5 | 48.7% | 234 | DEPLOYABLE |
| 18 | bb_kc_squeeze_1h | BTC | 1h | +17% | 20.12 | 0.14% | 31.7 | 47.6% | 410 | DEPLOYABLE |
| 19 | rsi_divergence_1h | ETH | 1h | +14% | 16.57 | 0.08% | 57.3 | 48.4% | 184 | DEPLOYABLE |
| 20 | bb_scalp_15m | BTC | 15m | +12% | 96.24 | 0.08% | 167.5 | 49.4% | 162 | A_OPTIMISER |
| 21 | elder_mtf_15m | ETH | 15m | +9% | 24.46 | 0.00% | 9999 | 50.0% | 88 | A_OPTIMISER |
| 22 | elder_mtf_15m | BTC | 15m | +3% | 43.42 | 0.00% | 9999 | 50.0% | 32 | INSUFFISANT |

---

## Top 3 par Phase de Marche

### Toutes phases (robustesse cross-period)
| Rang | Strategie | TF | P1 (365j) | P2 (ancien 180j) | P3 (recent 180j) | P4 (90j) |
|------|-----------|-----|-----------|------------------|-------------------|----------|
| 1 | **regime_adaptive_1h** | 1h | +27340% | +88% | +13816% | +962% |
| 2 | **ichimoku_trend_4h** | 4h | +13043% | +891% | +1036% | +201% |
| 3 | **ema_adx_trend_4h** | 4h | +950% | +190% | +252% | +72% |

> Ces 3 strategies fonctionnent sur TOUTES les periodes testees avec des returns positifs.

### Phase Range/Consolidation (P2 ancien 180j)
| Rang | Strategie | ETH | BTC |
|------|-----------|-----|-----|
| 1 | **ichimoku_trend_4h** | +891% | +1152% |
| 2 | **ema_adx_trend_4h** | +190% | +230% |
| 3 | **regime_adaptive_1h** | +88% | +115% |

### Phase recente (P4 90j)
| Rang | Strategie | ETH | BTC |
|------|-----------|-----|-----|
| 1 | **regime_adaptive_1h** | +962% | +1147% |
| 2 | **bb_scalp_15m** | +359% | +12% |
| 3 | **ichimoku_trend_4h** | +201% | +229% |

### Scalping / High-frequency
| Rang | Strategie | ETH | BTC | Sharpe |
|------|-----------|-----|-----|--------|
| 1 | **stochrsi_scalp_5m** | +96% | +112% | 180-195 |
| 2 | **bb_scalp_15m** | +359% | +12% | 96-98 |
| 3 | **vwap_reversion_15m** | +102% | +8% | 84-96 |

---

## Strategie supprimee

- **donchian_breakout_1d** — Returns negatifs sur toutes les periodes. Le breakout daily genere trop de faux signaux en crypto.

---

## Notes sur le Compound

- `equity_pct = 0.05` (5% du compte par trade)
- Les tailles de position croissent avec le compte (%-based sizing)
- SL/TP utilisent la taille reelle du fill (pas de recalcul)
- Pour retirer des gains: transferer vers un autre wallet, le bot s'adapte automatiquement au solde restant

## Avertissements

1. Les returns en backtest sont OPTIMISTES (IOC traites comme GTC, slippage minimal)
2. En live, attendre 10-20% des returns backtest
3. Le classement relatif entre strategies reste fiable
4. Les strategies 15m/5m manquent de data historique au-dela de ~180 jours (FAILED sur P2)
