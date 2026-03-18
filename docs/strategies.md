# Strategies de Trading — Documentation

> 1 strategie active en production : `sniper_1h.lua` sur ETH/BTC.
> Moteur C avec indicateurs natifs, Lua sandboxe, multi-coin automatique.

---

## Architecture commune

Chaque strategie suit le meme pattern :

- **COIN global** injecte par le moteur C avant chargement (fallback `"ETH"`)
- **Instance name** : `{strategie}_{coin}` (ex: `sniper_1h_eth`)
- **5 callbacks** : `on_init`, `on_tick`, `on_fill`, `on_timer`, `on_shutdown`
- **Entry IOC** : slippage 0.1% (`price * 1.001` buy, `* 0.999` sell)
- **SL/TP** : trigger orders reduce-only sur l'exchange
- **Etat persistant** : `trade_count`, `win_count`, `enabled` via `save_state`/`load_state`
- **Position recovery** : detecte les positions existantes au redemarrage
- **Max hold time** : fermeture forcee si la position est tenue trop longtemps

---

## Strategie active

### sniper_1h.lua — Sniper High-Conviction

**Type :** Ultra-selectif | **Timeframe :** 1h | **Coins :** ETH, BTC (SOL backup)

**Principe :** Signaux rares a haute conviction, levier eleve (x7), 90% equity par trade.

**Signaux ETH (4) :**
- **L1** : momentum + calm (RSI>65 + low_vol + MACD decel)
- **L3** : trend + calm (RSI>65 + ADX>25 + low_vol)
- **S1** : bear + calm (RSI<30 + low_vol + SMA20<50)
- **S2** : oversold + downtrend (RSI<30 + MACD<0 + low_vol)

**Signaux BTC (2) :**
- **L1** : momentum + calm — TP/SL 2.0%/2.0%, ATR<0.4%
- **S1** : bear + calm — TP/SL 2.0%/2.0%, ATR<0.4%

**Parametres :** Levier 7x, equity 90%, cooldown 4h, max_hold 48h, max_size $9999

**Backtest (5m simulation, 730j) :**
- ETH: +289.6%, 12 trades, 92% WR, DD 24.5%, PF 4.31
- BTC: +275.2%, 28 trades, 75% WR, DD 20.7%, PF 5.39

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
./build/backtest_json strategies/sniper_1h.lua ETH 0 90 1h

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
