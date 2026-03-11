# Backtest Report — OOS Transparent

**Date**: 2026-03-11 11:34

## Methode

- **Evaluation**: Out-of-Sample uniquement (40% des donnees, jamais vues par la strategie)
- **Walk-forward**: Split automatique 60% IS / 40% OOS
- **Fees**: Maker 0.02% (ALO entries), Taker 0.05% (trigger exits), Slippage 1bp
- **Capital**: $100, Levier 5x, Compound 10% de l'equity par trade
- **Benchmark**: Buy & Hold levier 5x sur la meme periode
- **Verdict OOS**: DEPLOYABLE (Sharpe>=1.5, alpha>0, PF>=1.5, DD<20%, 50+ trades) | A_OPTIMISER | MARGINAL | INSUFFISANT (<30 trades) | ABANDON

## Resultats Detailles (OOS uniquement)

| Strategie | TF | Coin | Jours | Trades | Return | Sharpe | Max DD | PF | WR | B&H | Alpha | Verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bb_kc_squeeze_1h | 1h | BTC | 180d | 122 | +13.1% | 19.87 | 0.3% | 42.89 | 48% | -112.3% | +125.4% | DEPLOYABLE |
| bb_kc_squeeze_1h | 1h | BTC | 90d | 56 | +7.4% | 19.88 | 0.3% | 28.76 | 46% | -56.4% | +63.8% | DEPLOYABLE |
| bb_kc_squeeze_1h | 1h | DOGE | 180d | 78 | +10.7% | 14.66 | 0.3% | 18.24 | 46% | -130.3% | +141.0% | DEPLOYABLE |
| bb_kc_squeeze_1h | 1h | DOGE | 90d | 34 | +5.9% | 16.94 | 0.0% | inf | 50% | -63.3% | +69.2% | A_OPTIMISER |
| bb_kc_squeeze_1h | 1h | ETH | 180d | 94 | +11.0% | 17.48 | 0.2% | 65.81 | 49% | -165.5% | +176.5% | DEPLOYABLE |
| bb_kc_squeeze_1h | 1h | ETH | 90d | 36 | +6.3% | 17.04 | 0.0% | inf | 50% | -66.5% | +72.8% | A_OPTIMISER |
| bb_kc_squeeze_1h | 1h | SOL | 180d | 74 | +8.0% | 14.22 | 0.2% | 19.57 | 46% | -165.3% | +173.3% | DEPLOYABLE |
| bb_kc_squeeze_1h | 1h | SOL | 90d | 20 | +3.3% | 12.94 | 0.0% | inf | 50% | -89.0% | +92.2% | INSUFFISANT |
| bb_scalp_15m | 15m | BTC | 45d | 716 | +176.4% | 107.57 | 0.3% | 119.94 | 49% | +16.2% | +160.2% | DEPLOYABLE |
| bb_scalp_15m | 15m | DOGE | 45d | 726 | +165.7% | 101.81 | 0.5% | 38.82 | 48% | -28.4% | +194.1% | DEPLOYABLE |
| bb_scalp_15m | 15m | ETH | 45d | 730 | +177.1% | 106.54 | 0.3% | 77.10 | 49% | +18.0% | +159.1% | DEPLOYABLE |
| bb_scalp_15m | 15m | SOL | 45d | 784 | +194.4% | 107.98 | 0.3% | 61.73 | 48% | +9.9% | +184.6% | DEPLOYABLE |
| donchian_breakout_1d | 1d | BTC | 365d | 0 | +0.0% | 0.00 | 0.0% | 0.00 | 0% | +0.0% | +0.0% | FAILED |
| donchian_breakout_1d | 1d | BTC | 180d | 0 | +0.0% | 0.00 | 0.0% | 0.00 | 0% | +0.0% | +0.0% | FAILED |
| donchian_breakout_1d | 1d | DOGE | 365d | 0 | +0.0% | 0.00 | 0.0% | 0.00 | 0% | +0.0% | +0.0% | FAILED |
| donchian_breakout_1d | 1d | DOGE | 180d | 0 | +0.0% | 0.00 | 0.0% | 0.00 | 0% | +0.0% | +0.0% | FAILED |
| donchian_breakout_1d | 1d | ETH | 365d | 0 | +0.0% | 0.00 | 0.0% | 0.00 | 0% | +0.0% | +0.0% | FAILED |
| donchian_breakout_1d | 1d | ETH | 180d | 0 | +0.0% | 0.00 | 0.0% | 0.00 | 0% | +0.0% | +0.0% | FAILED |
| donchian_breakout_1d | 1d | SOL | 365d | 0 | +0.0% | 0.00 | 0.0% | 0.00 | 0% | +0.0% | +0.0% | FAILED |
| donchian_breakout_1d | 1d | SOL | 180d | 0 | +0.0% | 0.00 | 0.0% | 0.00 | 0% | +0.0% | +0.0% | FAILED |
| elder_mtf_15m | 15m | BTC | 45d | 40 | +8.1% | 28.11 | 0.0% | inf | 50% | +16.2% | -8.1% | MARGINAL |
| elder_mtf_15m | 15m | DOGE | 45d | 110 | +24.0% | 46.83 | 0.0% | inf | 50% | -28.4% | +52.4% | DEPLOYABLE |
| elder_mtf_15m | 15m | ETH | 45d | 64 | +13.3% | 35.70 | 0.0% | inf | 50% | +18.0% | -4.7% | A_OPTIMISER |
| elder_mtf_15m | 15m | SOL | 45d | 46 | +9.4% | 30.19 | 0.0% | inf | 50% | +9.9% | -0.5% | A_OPTIMISER |
| ema_adx_trend_4h | 4h | BTC | 365d | 416 | +219.0% | 26.89 | 0.5% | 49.17 | 48% | -185.9% | +405.0% | DEPLOYABLE |
| ema_adx_trend_4h | 4h | BTC | 180d | 230 | +90.6% | 30.37 | 0.5% | 49.74 | 48% | -112.1% | +202.7% | DEPLOYABLE |
| ema_adx_trend_4h | 4h | BTC | 90d | 62 | +15.1% | 17.90 | 0.5% | 12.26 | 42% | -55.8% | +70.9% | DEPLOYABLE |
| ema_adx_trend_4h | 4h | DOGE | 365d | 168 | +50.9% | 15.87 | 0.5% | 17.28 | 44% | -260.2% | +311.1% | DEPLOYABLE |
| ema_adx_trend_4h | 4h | DOGE | 180d | 106 | +28.9% | 17.61 | 0.5% | 15.64 | 43% | -130.8% | +159.6% | DEPLOYABLE |
| ema_adx_trend_4h | 4h | DOGE | 90d | 38 | +9.1% | 14.03 | 0.3% | 12.45 | 42% | -61.7% | +70.8% | A_OPTIMISER |
| ema_adx_trend_4h | 4h | ETH | 365d | 262 | +100.7% | 21.51 | 0.5% | 32.72 | 47% | -247.1% | +347.8% | DEPLOYABLE |
| ema_adx_trend_4h | 4h | ETH | 180d | 180 | +64.4% | 26.86 | 0.3% | 45.27 | 48% | -166.5% | +230.9% | DEPLOYABLE |
| ema_adx_trend_4h | 4h | ETH | 90d | 28 | +6.8% | 12.35 | 0.3% | 13.88 | 43% | -66.8% | +73.5% | INSUFFISANT |
| ema_adx_trend_4h | 4h | SOL | 365d | 154 | +54.9% | 18.37 | 0.3% | 106.66 | 49% | -277.7% | +332.6% | DEPLOYABLE |
| ema_adx_trend_4h | 4h | SOL | 180d | 110 | +38.4% | 23.30 | 0.0% | inf | 50% | -165.3% | +203.7% | DEPLOYABLE |
| ema_adx_trend_4h | 4h | SOL | 90d | 4 | +1.2% | 5.52 | 0.0% | inf | 50% | -86.7% | +87.9% | INSUFFISANT |
| ichimoku_trend_4h | 4h | BTC | 365d | 754 | +1773.7% | 28.45 | 0.3% | 287.05 | 50% | -185.9% | +1959.7% | DEPLOYABLE |
| ichimoku_trend_4h | 4h | BTC | 180d | 364 | +315.8% | 36.83 | 0.3% | 340.66 | 50% | -112.1% | +427.9% | DEPLOYABLE |
| ichimoku_trend_4h | 4h | BTC | 90d | 164 | +88.9% | 38.49 | 0.3% | 211.48 | 49% | -55.8% | +144.7% | DEPLOYABLE |
| ichimoku_trend_4h | 4h | DOGE | 365d | 330 | +225.9% | 23.02 | 0.3% | 36.28 | 47% | -260.2% | +486.1% | DEPLOYABLE |
| ichimoku_trend_4h | 4h | DOGE | 180d | 138 | +65.0% | 23.09 | 0.3% | 40.45 | 47% | -130.8% | +195.7% | DEPLOYABLE |
| ichimoku_trend_4h | 4h | DOGE | 90d | 52 | +20.1% | 19.48 | 0.3% | 29.17 | 46% | -61.7% | +81.8% | DEPLOYABLE |
| ichimoku_trend_4h | 4h | ETH | 365d | 438 | +432.6% | 26.35 | 0.3% | 90.09 | 49% | -247.1% | +679.7% | DEPLOYABLE |
| ichimoku_trend_4h | 4h | ETH | 180d | 232 | +141.7% | 30.58 | 0.3% | 80.33 | 49% | -166.5% | +308.1% | DEPLOYABLE |
| ichimoku_trend_4h | 4h | ETH | 90d | 80 | +34.1% | 25.58 | 0.3% | 50.35 | 48% | -66.8% | +100.9% | DEPLOYABLE |
| ichimoku_trend_4h | 4h | SOL | 365d | 308 | +212.1% | 23.35 | 0.3% | 62.39 | 48% | -277.7% | +489.7% | DEPLOYABLE |
| ichimoku_trend_4h | 4h | SOL | 180d | 90 | +41.0% | 20.09 | 0.3% | 98.32 | 49% | -165.3% | +206.4% | DEPLOYABLE |
| ichimoku_trend_4h | 4h | SOL | 90d | 14 | +4.5% | 8.72 | 0.3% | 15.50 | 43% | -86.7% | +91.2% | INSUFFISANT |
| macd_momentum_1h | 1h | BTC | 180d | 1156 | +219.8% | 54.16 | 0.2% | 44.87 | 48% | -112.3% | +332.1% | DEPLOYABLE |
| macd_momentum_1h | 1h | BTC | 90d | 550 | +93.2% | 62.99 | 0.2% | 45.77 | 48% | -56.4% | +149.6% | DEPLOYABLE |
| macd_momentum_1h | 1h | DOGE | 180d | 1086 | +371.5% | 52.71 | 0.3% | 32.09 | 47% | -130.3% | +501.8% | DEPLOYABLE |
| macd_momentum_1h | 1h | DOGE | 90d | 476 | +106.2% | 58.37 | 0.2% | 35.01 | 47% | -63.3% | +169.5% | DEPLOYABLE |
| macd_momentum_1h | 1h | ETH | 180d | 1182 | +360.4% | 51.94 | 0.3% | 34.44 | 48% | -165.5% | +525.9% | DEPLOYABLE |
| macd_momentum_1h | 1h | ETH | 90d | 584 | +137.3% | 62.08 | 0.2% | 31.73 | 47% | -66.5% | +203.8% | DEPLOYABLE |
| macd_momentum_1h | 1h | SOL | 180d | 1170 | +453.0% | 51.87 | 0.4% | 45.76 | 48% | -165.3% | +618.3% | DEPLOYABLE |
| macd_momentum_1h | 1h | SOL | 90d | 552 | +154.0% | 62.72 | 0.4% | 46.37 | 48% | -89.0% | +242.9% | DEPLOYABLE |
| regime_adaptive_1h | 1h | BTC | 180d | 1790 | +2837.7% | 57.80 | 0.5% | 275.25 | 50% | -112.3% | +2950.0% | DEPLOYABLE |
| regime_adaptive_1h | 1h | BTC | 90d | 874 | +419.5% | 76.80 | 0.2% | 329.44 | 50% | -56.4% | +475.9% | DEPLOYABLE |
| regime_adaptive_1h | 1h | DOGE | 180d | 1482 | +1293.2% | 56.07 | 0.4% | 48.62 | 48% | -130.3% | +1423.5% | DEPLOYABLE |
| regime_adaptive_1h | 1h | DOGE | 90d | 644 | +204.9% | 65.44 | 0.4% | 41.87 | 47% | -63.3% | +268.2% | DEPLOYABLE |
| regime_adaptive_1h | 1h | ETH | 180d | 1698 | +2078.8% | 56.34 | 0.5% | 49.87 | 48% | -165.5% | +2244.4% | DEPLOYABLE |
| regime_adaptive_1h | 1h | ETH | 90d | 744 | +265.3% | 68.53 | 0.4% | 41.72 | 47% | -66.5% | +331.7% | DEPLOYABLE |
| regime_adaptive_1h | 1h | SOL | 180d | 1628 | +1777.0% | 56.74 | 0.3% | 61.90 | 49% | -165.3% | +1942.2% | DEPLOYABLE |
| regime_adaptive_1h | 1h | SOL | 90d | 740 | +273.1% | 70.22 | 0.2% | 60.03 | 48% | -89.0% | +362.1% | DEPLOYABLE |
| rsi_divergence_1h | 1h | BTC | 180d | 68 | +9.4% | 16.10 | 0.2% | 30.83 | 47% | -112.3% | +121.7% | DEPLOYABLE |
| rsi_divergence_1h | 1h | BTC | 90d | 28 | +3.2% | 12.00 | 0.2% | 11.89 | 43% | -56.4% | +59.6% | INSUFFISANT |
| rsi_divergence_1h | 1h | DOGE | 180d | 64 | +7.8% | 13.50 | 0.2% | 13.88 | 44% | -130.3% | +138.1% | DEPLOYABLE |
| rsi_divergence_1h | 1h | DOGE | 90d | 40 | +5.0% | 15.89 | 0.2% | 17.79 | 45% | -63.3% | +68.3% | A_OPTIMISER |
| rsi_divergence_1h | 1h | ETH | 180d | 68 | +9.4% | 16.17 | 0.2% | 32.31 | 47% | -165.5% | +175.0% | DEPLOYABLE |
| rsi_divergence_1h | 1h | ETH | 90d | 42 | +6.3% | 20.38 | 0.0% | inf | 50% | -66.5% | +72.8% | A_OPTIMISER |
| rsi_divergence_1h | 1h | SOL | 180d | 42 | +5.8% | 12.94 | 0.2% | 39.28 | 48% | -165.3% | +171.1% | A_OPTIMISER |
| rsi_divergence_1h | 1h | SOL | 90d | 20 | +2.5% | 11.11 | 0.2% | 17.95 | 45% | -89.0% | +91.5% | INSUFFISANT |
| stochrsi_scalp_5m | 5m | BTC | 15d | 552 | +47.9% | 180.80 | 0.0% | inf | 50% | -19.8% | +67.7% | DEPLOYABLE |
| stochrsi_scalp_5m | 5m | DOGE | 15d | 388 | +29.6% | 140.15 | 0.2% | 49.07 | 48% | -13.3% | +42.9% | DEPLOYABLE |
| stochrsi_scalp_5m | 5m | ETH | 15d | 500 | +41.2% | 166.33 | 0.1% | 117.94 | 49% | -23.7% | +64.9% | DEPLOYABLE |
| stochrsi_scalp_5m | 5m | SOL | 15d | 162 | +10.9% | 147.53 | 0.3% | 26.89 | 47% | -40.0% | +50.9% | DEPLOYABLE |
| triple_confirm_15m | 15m | BTC | 45d | 350 | +49.9% | 78.04 | 0.1% | 70.26 | 49% | +16.2% | +33.6% | DEPLOYABLE |
| triple_confirm_15m | 15m | DOGE | 45d | 778 | +136.9% | 105.61 | 0.4% | 34.39 | 47% | -28.4% | +165.3% | DEPLOYABLE |
| triple_confirm_15m | 15m | ETH | 45d | 302 | +39.3% | 67.94 | 0.1% | 31.90 | 47% | +18.0% | +21.3% | DEPLOYABLE |
| triple_confirm_15m | 15m | SOL | 45d | 340 | +46.4% | 73.95 | 0.3% | 41.29 | 48% | +9.9% | +36.5% | DEPLOYABLE |
| vwap_reversion_15m | 15m | BTC | 45d | 586 | +69.2% | 97.37 | 0.2% | 46.61 | 48% | +16.2% | +53.0% | DEPLOYABLE |
| vwap_reversion_15m | 15m | DOGE | 45d | 686 | +77.5% | 95.37 | 0.3% | 21.06 | 46% | -28.4% | +105.9% | DEPLOYABLE |
| vwap_reversion_15m | 15m | ETH | 45d | 628 | +68.9% | 92.02 | 0.2% | 22.02 | 46% | +18.0% | +50.9% | DEPLOYABLE |
| vwap_reversion_15m | 15m | SOL | 45d | 626 | +68.1% | 91.07 | 0.2% | 20.80 | 46% | +9.9% | +58.2% | DEPLOYABLE |
| williams_obv_4h | 4h | BTC | 365d | 106 | +27.9% | 14.96 | 0.2% | 61.26 | 48% | -185.9% | +213.9% | DEPLOYABLE |
| williams_obv_4h | 4h | BTC | 180d | 30 | +6.9% | 10.62 | 0.2% | 34.00 | 47% | -112.1% | +119.0% | A_OPTIMISER |
| williams_obv_4h | 4h | BTC | 90d | 12 | +2.3% | 7.60 | 0.2% | 12.40 | 42% | -55.8% | +58.1% | INSUFFISANT |
| williams_obv_4h | 4h | DOGE | 365d | 68 | +11.7% | 8.00 | 0.6% | 7.85 | 38% | -260.2% | +271.9% | DEPLOYABLE |
| williams_obv_4h | 4h | DOGE | 180d | 38 | +5.3% | 7.05 | 0.6% | 5.32 | 34% | -130.8% | +136.0% | A_OPTIMISER |
| williams_obv_4h | 4h | DOGE | 90d | 4 | -0.4% | -5.52 | 0.4% | 0.00 | 0% | -61.7% | +61.3% | INSUFFISANT |
| williams_obv_4h | 4h | ETH | 365d | 46 | +10.4% | 8.93 | 0.2% | 25.86 | 46% | -247.1% | +257.5% | A_OPTIMISER |
| williams_obv_4h | 4h | ETH | 180d | 22 | +4.8% | 8.63 | 0.2% | 24.74 | 46% | -166.5% | +171.3% | INSUFFISANT |
| williams_obv_4h | 4h | ETH | 90d | 10 | +2.5% | 9.55 | 0.0% | inf | 50% | -66.8% | +69.2% | INSUFFISANT |
| williams_obv_4h | 4h | SOL | 365d | 42 | +8.6% | 7.64 | 0.2% | 14.89 | 43% | -277.7% | +286.2% | A_OPTIMISER |
| williams_obv_4h | 4h | SOL | 180d | 16 | +3.3% | 6.81 | 0.2% | 17.04 | 44% | -165.3% | +168.6% | INSUFFISANT |
| williams_obv_4h | 4h | SOL | 90d | 2 | -0.2% | -3.18 | 0.2% | 0.00 | 0% | -86.7% | +86.5% | INSUFFISANT |

## Analyse Overfitting (IS vs OOS)

| Strategie | Coin | Jours | Sharpe IS | Sharpe OOS | Decay | Signal |
|---|---|---|---|---|---|---|
| bb_kc_squeeze_1h | BTC | 180d | 19.21 | 19.87 | 4% | OK |
| bb_kc_squeeze_1h | BTC | 90d | 20.87 | 19.88 | -5% | OK |
| bb_kc_squeeze_1h | DOGE | 180d | 14.66 | 14.66 | -0% | OK |
| bb_kc_squeeze_1h | DOGE | 90d | 15.14 | 16.94 | 12% | OK |
| bb_kc_squeeze_1h | ETH | 180d | 19.02 | 17.48 | -8% | OK |
| bb_kc_squeeze_1h | ETH | 90d | 17.40 | 17.04 | -2% | OK |
| bb_kc_squeeze_1h | SOL | 180d | 13.20 | 14.22 | 8% | OK |
| bb_kc_squeeze_1h | SOL | 90d | 15.39 | 12.94 | -16% | OK |
| bb_scalp_15m | BTC | 45d | 98.41 | 107.57 | 9% | OK |
| bb_scalp_15m | DOGE | 45d | 95.06 | 101.81 | 7% | OK |
| bb_scalp_15m | ETH | 45d | 97.09 | 106.54 | 10% | OK |
| bb_scalp_15m | SOL | 45d | 97.01 | 107.98 | 11% | OK |
| donchian_breakout_1d | BTC | 365d | 0.00 | 0.00 | 0% | OK |
| donchian_breakout_1d | BTC | 180d | 0.00 | 0.00 | 0% | OK |
| donchian_breakout_1d | DOGE | 365d | 0.00 | 0.00 | 0% | OK |
| donchian_breakout_1d | DOGE | 180d | 0.00 | 0.00 | 0% | OK |
| donchian_breakout_1d | ETH | 365d | 0.00 | 0.00 | 0% | OK |
| donchian_breakout_1d | ETH | 180d | 0.00 | 0.00 | 0% | OK |
| donchian_breakout_1d | SOL | 365d | 0.00 | 0.00 | 0% | OK |
| donchian_breakout_1d | SOL | 180d | 0.00 | 0.00 | 0% | OK |
| elder_mtf_15m | BTC | 45d | 22.95 | 28.11 | 22% | OK |
| elder_mtf_15m | DOGE | 45d | 24.65 | 46.83 | 90% | OK |
| elder_mtf_15m | ETH | 45d | 18.38 | 35.70 | 94% | OK |
| elder_mtf_15m | SOL | 45d | 28.68 | 30.19 | 5% | OK |
| ema_adx_trend_4h | BTC | 365d | 26.70 | 26.89 | 1% | OK |
| ema_adx_trend_4h | BTC | 180d | 31.99 | 30.37 | -5% | OK |
| ema_adx_trend_4h | BTC | 90d | 36.35 | 17.90 | -51% | OVERFIT |
| ema_adx_trend_4h | DOGE | 365d | 11.29 | 15.87 | 40% | OK |
| ema_adx_trend_4h | DOGE | 180d | 11.42 | 17.61 | 54% | OK |
| ema_adx_trend_4h | DOGE | 90d | 21.68 | 14.03 | -35% | SUSPECT |
| ema_adx_trend_4h | ETH | 365d | 18.39 | 21.51 | 17% | OK |
| ema_adx_trend_4h | ETH | 180d | 20.32 | 26.86 | 32% | OK |
| ema_adx_trend_4h | ETH | 90d | 32.27 | 12.35 | -62% | OVERFIT |
| ema_adx_trend_4h | SOL | 365d | 11.59 | 18.37 | 58% | OK |
| ema_adx_trend_4h | SOL | 180d | 8.63 | 23.30 | 170% | OK |
| ema_adx_trend_4h | SOL | 90d | 28.07 | 5.52 | -80% | OVERFIT |
| ichimoku_trend_4h | BTC | 365d | 22.70 | 28.45 | 25% | OK |
| ichimoku_trend_4h | BTC | 180d | 32.21 | 36.83 | 14% | OK |
| ichimoku_trend_4h | BTC | 90d | 40.48 | 38.49 | -5% | OK |
| ichimoku_trend_4h | DOGE | 365d | 19.52 | 23.02 | 18% | OK |
| ichimoku_trend_4h | DOGE | 180d | 23.20 | 23.09 | -0% | OK |
| ichimoku_trend_4h | DOGE | 90d | 25.94 | 19.48 | -25% | OK |
| ichimoku_trend_4h | ETH | 365d | 22.42 | 26.35 | 18% | OK |
| ichimoku_trend_4h | ETH | 180d | 24.75 | 30.58 | 24% | OK |
| ichimoku_trend_4h | ETH | 90d | 38.05 | 25.58 | -33% | SUSPECT |
| ichimoku_trend_4h | SOL | 365d | 20.55 | 23.35 | 14% | OK |
| ichimoku_trend_4h | SOL | 180d | 22.57 | 20.09 | -11% | OK |
| ichimoku_trend_4h | SOL | 90d | 32.19 | 8.72 | -73% | OVERFIT |
| macd_momentum_1h | BTC | 180d | 54.18 | 54.16 | -0% | OK |
| macd_momentum_1h | BTC | 90d | 53.19 | 62.99 | 18% | OK |
| macd_momentum_1h | DOGE | 180d | 51.19 | 52.71 | 3% | OK |
| macd_momentum_1h | DOGE | 90d | 54.39 | 58.37 | 7% | OK |
| macd_momentum_1h | ETH | 180d | 51.37 | 51.94 | 1% | OK |
| macd_momentum_1h | ETH | 90d | 54.54 | 62.08 | 14% | OK |
| macd_momentum_1h | SOL | 180d | 51.35 | 51.87 | 1% | OK |
| macd_momentum_1h | SOL | 90d | 54.45 | 62.72 | 15% | OK |
| regime_adaptive_1h | BTC | 180d | 46.00 | 57.80 | 26% | OK |
| regime_adaptive_1h | BTC | 90d | 66.03 | 76.80 | 16% | OK |
| regime_adaptive_1h | DOGE | 180d | 46.00 | 56.07 | 22% | OK |
| regime_adaptive_1h | DOGE | 90d | 64.92 | 65.44 | 1% | OK |
| regime_adaptive_1h | ETH | 180d | 45.85 | 56.34 | 23% | OK |
| regime_adaptive_1h | ETH | 90d | 65.87 | 68.53 | 4% | OK |
| regime_adaptive_1h | SOL | 180d | 45.50 | 56.74 | 25% | OK |
| regime_adaptive_1h | SOL | 90d | 64.56 | 70.22 | 9% | OK |
| rsi_divergence_1h | BTC | 180d | 17.09 | 16.10 | -6% | OK |
| rsi_divergence_1h | BTC | 90d | 19.62 | 12.00 | -39% | SUSPECT |
| rsi_divergence_1h | DOGE | 180d | 14.56 | 13.50 | -7% | OK |
| rsi_divergence_1h | DOGE | 90d | 11.51 | 15.89 | 38% | OK |
| rsi_divergence_1h | ETH | 180d | 15.26 | 16.17 | 6% | OK |
| rsi_divergence_1h | ETH | 90d | 10.91 | 20.38 | 87% | OK |
| rsi_divergence_1h | SOL | 180d | 15.51 | 12.94 | -16% | OK |
| rsi_divergence_1h | SOL | 90d | 13.00 | 11.11 | -15% | OK |
| stochrsi_scalp_5m | BTC | 15d | 164.26 | 180.80 | 10% | OK |
| stochrsi_scalp_5m | DOGE | 15d | 150.44 | 140.15 | -7% | OK |
| stochrsi_scalp_5m | ETH | 15d | 155.02 | 166.33 | 7% | OK |
| stochrsi_scalp_5m | SOL | 15d | 116.28 | 147.53 | 27% | OK |
| triple_confirm_15m | BTC | 45d | 74.38 | 78.04 | 5% | OK |
| triple_confirm_15m | DOGE | 45d | 91.56 | 105.61 | 15% | OK |
| triple_confirm_15m | ETH | 45d | 66.84 | 67.94 | 2% | OK |
| triple_confirm_15m | SOL | 45d | 69.06 | 73.95 | 7% | OK |
| vwap_reversion_15m | BTC | 45d | 87.04 | 97.37 | 12% | OK |
| vwap_reversion_15m | DOGE | 45d | 77.36 | 95.37 | 23% | OK |
| vwap_reversion_15m | ETH | 45d | 82.55 | 92.02 | 12% | OK |
| vwap_reversion_15m | SOL | 45d | 78.32 | 91.07 | 16% | OK |
| williams_obv_4h | BTC | 365d | 16.40 | 14.96 | -9% | OK |
| williams_obv_4h | BTC | 180d | 16.93 | 10.62 | -37% | SUSPECT |
| williams_obv_4h | BTC | 90d | 18.54 | 7.60 | -59% | OVERFIT |
| williams_obv_4h | DOGE | 365d | 6.69 | 8.00 | 20% | OK |
| williams_obv_4h | DOGE | 180d | 7.18 | 7.05 | -2% | OK |
| williams_obv_4h | DOGE | 90d | 12.18 | -5.52 | -145% | OVERFIT |
| williams_obv_4h | ETH | 365d | 11.07 | 8.93 | -19% | OK |
| williams_obv_4h | ETH | 180d | 8.74 | 8.63 | -1% | OK |
| williams_obv_4h | ETH | 90d | 12.34 | 9.55 | -23% | OK |
| williams_obv_4h | SOL | 365d | 9.29 | 7.64 | -18% | OK |
| williams_obv_4h | SOL | 180d | 7.62 | 6.81 | -11% | OK |
| williams_obv_4h | SOL | 90d | 13.37 | -3.18 | -124% | OVERFIT |

## Classement Final (moyenne OOS sur tous les coins/periodes)

| Rang | Strategie | TF | Sharpe Moy | Return Moy | Alpha Moy | Max DD | Deploy |
|---|---|---|---|---|---|---|---|
| 1 | stochrsi_scalp_5m | 5m | 158.70 | +32.4% | +56.6% | 0.3% | 4/4 |
| 2 | bb_scalp_15m | 15m | 105.98 | +178.4% | +174.5% | 0.5% | 4/4 |
| 3 | vwap_reversion_15m | 15m | 93.96 | +70.9% | +67.0% | 0.3% | 4/4 |
| 4 | triple_confirm_15m | 15m | 81.39 | +68.1% | +64.2% | 0.4% | 4/4 |
| 5 | regime_adaptive_1h | 1h | 63.49 | +1143.7% | +1249.8% | 0.5% | 8/8 |
| 6 | macd_momentum_1h | 1h | 57.11 | +236.9% | +343.0% | 0.4% | 8/8 |
| 7 | elder_mtf_15m | 15m | 35.21 | +13.7% | +9.8% | 0.0% | 1/4 |
| 8 | ichimoku_trend_4h | 4h | 26.85 | +304.6% | +461.9% | 0.3% | 11/11 |
| 9 | ema_adx_trend_4h | 4h | 21.27 | +67.2% | +233.5% | 0.5% | 9/10 |
| 10 | bb_kc_squeeze_1h | 1h | 17.16 | +8.9% | +117.4% | 0.3% | 5/7 |
| 11 | rsi_divergence_1h | 1h | 15.83 | +7.3% | +124.5% | 0.2% | 3/6 |
| 12 | williams_obv_4h | 4h | 9.53 | +11.8% | +214.1% | 0.6% | 2/6 |

**Non classees** (pas assez de trades ou FAILED): donchian_breakout_1d

## Notes

- Sharpe > 5 = suspect (overfitting probable ou trop peu de trades)
- Alpha = Return strategie - Return Buy & Hold (meme periode, meme levier 5x)
- Periodes limitees par l'API Hyperliquid: 5m=17j, 15m=52j, 1h=208j, 4h=833j
- Walk-forward: 60% IS (calibration) / 40% OOS (evaluation)
- Les resultats passes ne garantissent pas les performances futures
