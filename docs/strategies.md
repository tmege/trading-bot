# Strategies de Trading — Documentation

> 1 strategie active en production : `regime_adaptive_1h.lua` sur BTC/ETH/SOL.
> Moteur C avec indicateurs natifs, Lua sandboxe, multi-coin automatique.

---

## Architecture commune

Chaque strategie suit le meme pattern :

- **COIN global** injecte par le moteur C avant chargement (fallback `"ETH"`)
- **Instance name** : `{strategie}_{coin}` (ex: `regime_adaptive_1h_btc`)
- **5 callbacks** : `on_init`, `on_tick`, `on_fill`, `on_timer`, `on_shutdown`
- **Entry IOC** : slippage 0.1% (`price * 1.001` buy, `* 0.999` sell)
- **SL/TP** : trigger orders reduce-only sur l'exchange
- **Etat persistant** : `trade_count`, `win_count`, `enabled` via `save_state`/`load_state`
- **Position recovery** : detecte les positions existantes au redemarrage
- **Max hold time** : fermeture forcee si la position est tenue trop longtemps
- **Sizing** : 15% equity par trade, levier 5x max

---

## Strategie active

### regime_adaptive_1h.lua — Regime Adaptatif

**Type :** Adaptatif (2 modes) | **Timeframe :** 1h | **Coins :** BTC, ETH, SOL

**Detection de regime :**
- **Low Vol** (BB width < 3% ET ADX < 20) → mode Mean Reversion
- **High Vol** (BB width > 5% OU ADX > 25) → mode Trend Following
- **Transition** : ferme la position si le regime change

**Mode Mean Reversion :**
- LONG : prix < BB lower + RSI < 35 → SL 1.2%, TP 2%
- SHORT : prix > BB upper + RSI > 65

**Mode Trend Following :**
- LONG : EMA(12) > EMA(26) + ADX > 25 → SL 2%, TP 4%
- SHORT : EMA(12) < EMA(26) + ADX > 25

**Confirmations additionnelles :**
- MACD histogram direction (momentum)
- OBV vs OBV SMA (volume)
- ATR-based SL/TP dynamiques
- Liquidation bounce detection

**Concept :** S'adapte dynamiquement au regime de marche. Evite d'appliquer du mean reversion en marche tendanciel et inversement.

**Parametres :** Levier 5x, equity 15%, max_size $120, check 60s, cooldown 120s, max hold 4h

**Backtest (5m simulation, walk-forward 60/40):**
- Seule strategie avec des verdicts DEPLOYABLE (6/27 periodes)
- Meilleur sur ETH (+1.0% moy) et en marche bear/high-vol
- Performance en low-vol a ameliorer

---

## Indicateurs C disponibles

Tous les indicateurs sont calcules nativement en C et exposes via `bot.get_indicators(coin, interval, count, live_price)` :

| Indicateur | Champ Lua | Params |
|-----------|-----------|--------|
| SMA | `sma_20`, `sma_50`, `sma_200` | 20, 50, 200 |
| EMA | `ema_12`, `ema_26` | 12, 26 |
| RSI | `rsi` | 14 |
| MACD | `macd`, `macd_signal`, `macd_histogram` | 12, 26, 9 |
| Bollinger Bands | `bb_upper`, `bb_middle`, `bb_lower`, `bb_width` | 20, 2.0 |
| ATR | `atr` | 14 |
| VWAP | `vwap` | cumulatif |
| ADX | `adx`, `plus_di`, `minus_di` | 14, Wilder |
| Keltner Channels | `kc_upper`, `kc_middle`, `kc_lower` | EMA 20, ATR 10, 1.5x |
| Donchian Channels | `dc_upper`, `dc_lower`, `dc_middle` | 20 |
| Stochastic RSI | `stoch_rsi_k`, `stoch_rsi_d` | RSI 14, Stoch 14, K=3, D=3 |
| CCI | `cci` | 20 |
| Williams %R | `williams_r` | 14 |
| OBV | `obv`, `obv_sma` | cumul + SMA 20 |
| Ichimoku | `ichi_tenkan`, `ichi_kijun`, `ichi_senkou_a`, `ichi_senkou_b`, `ichi_chikou` | 9, 26, 52 |

**Signaux derives :** `above_sma200`, `golden_cross`, `rsi_oversold`, `rsi_overbought`, `bb_squeeze`, `macd_bullish`, `adx_trending`, `kc_squeeze`, `ichi_bullish`

---

## Backtest

```bash
# Une seule strategie
./build/backtest_json strategies/regime_adaptive_1h.lua ETH 0 90 1h

# Toutes les periodes de marche
bash data/backtest_results/market_periods/run_all.sh

# Batch report
./scripts/backtest_all.sh 365 0
```

Le runner effectue un walk-forward 60% IS / 40% OOS avec verdicts automatiques :
- **DEPLOYABLE** : Sharpe OOS >= 1.5, PF >= 1.5, alpha > 0, DD < 20%
- **A_OPTIMISER** : Sharpe >= 0.8, PF >= 1.2
- **MARGINAL** : profitable mais metriques faibles
- **INSUFFISANT** : < 30 trades
- **ABANDON** : perte, DD > 50%, ou Sharpe < 0.5
