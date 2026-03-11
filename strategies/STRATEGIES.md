# Strategies de Trading — Documentation

> 12 strategies backtestables sur ETH/BTC, conçues pour surpasser la baseline `bb_scalp_15m.lua`.
> Toutes utilisent le moteur C avec indicateurs natifs, Lua sandboxe, et multi-coin automatique.

---

## Architecture commune

Chaque strategie suit le meme pattern :

- **COIN global** injecte par le moteur C avant chargement (fallback `"ETH"`)
- **Instance name** : `{strategie}_{coin}` (ex: `triple_confirm_15m_eth`)
- **6 callbacks** : `on_init`, `on_tick`, `on_fill`, `on_timer`, `on_advisory`, `on_shutdown`
- **Entry IOC** : slippage 0.1% (`price * 1.001` buy, `* 0.999` sell)
- **SL/TP** : trigger orders reduce-only sur l'exchange
- **Etat persistant** : `trade_count`, `win_count`, `enabled` via `save_state`/`load_state`
- **Advisory** : pause/resume par section JSON specifique a l'instance
- **Position recovery** : detecte les positions existantes au redemarrage
- **Max hold time** : fermeture forcee si la position est tenue trop longtemps
- **Sizing** : $40 par trade, cap $60, levier variable selon la strategie

---

## Vue d'ensemble

| # | Strategie | Type | TF | Levier | SL | TP | Cooldown |
|---|-----------|------|----|--------|-----|-----|----------|
| 0 | `bb_scalp_15m` | Mean Reversion | 15m | 5x | 1.5% | 3.0% | 60s |
| 1 | `triple_confirm_15m` | Mean Reversion | 15m | 5x | 1.2% | 2.5% | 60s |
| 2 | `macd_momentum_1h` | Momentum | 1h | 5x | 1.5x ATR | 3x ATR | 120s |
| 3 | `rsi_divergence_1h` | Divergence | 1h | 5x | 1.5% | 3.0% | 300s |
| 4 | `ema_adx_trend_4h` | Trend Following | 4h | 3x | 2.5% | 6.0% | 600s |
| 5 | `bb_kc_squeeze_1h` | Breakout | 1h | 3x | 2x ATR | 3x ATR | 120s |
| 6 | `donchian_breakout_1d` | Breakout/Trend | 1d | 2x | 2x ATR | trail | 3600s |
| 7 | `vwap_reversion_15m` | Mean Reversion | 15m | 5x | 1.0% | VWAP | 60s |
| 8 | `stochrsi_scalp_5m` | Scalping | 5m | 5x | 0.8% | 1.5% | 30s |
| 9 | `williams_obv_4h` | Swing | 4h | 3x | 2.0% | 5.0% | 600s |
| 10 | `regime_adaptive_1h` | Adaptive | 1h | 5x | var | var | 120s |
| 11 | `elder_mtf_15m` | Multi-TF | 15m | 5x | 2.0% | 4.0% | 120s |
| 12 | `ichimoku_trend_4h` | Trend | 4h | 3x | 3.0% | 8.0% | 600s |

---

## Strategies detaillees

### 0. bb_scalp_15m.lua — Baseline BB Scalping

**Type :** Mean Reversion | **Timeframe :** 15m

**Logique :**
- LONG : prix touche la bande basse BB + RSI < 40
- SHORT : prix touche la bande haute BB + RSI > 60
- Exit : retour a la bande moyenne BB (conservateur)

**Filtres :** BB width entre 1% et 8% (evite marches plats et tendances fortes)

**Parametres :** SL 1.5%, TP 3.0%, BB(20, 2.0), levier 5x

---

### 1. triple_confirm_15m.lua — Triple Confirmation

**Type :** Mean Reversion amelioree | **Timeframe :** 15m

**Logique :**
- LONG : prix <= BB lower + RSI < 30 + MACD histogram **montant** (momentum tourne)
- SHORT : prix >= BB upper + RSI > 70 + MACD histogram **descendant**

**Avantage vs baseline :** Le 3eme filtre (MACD direction) elimine ~30% des faux signaux. RSI plus strict (30/70 vs 40/60).

**Filtres :** BB width 1.5-6%, tracking du `prev_histogram` pour detecter la direction

**Parametres :** SL 1.2%, TP 2.5%, check 5s, cooldown 60s, max hold 1h

---

### 2. macd_momentum_1h.lua — MACD Momentum

**Type :** Momentum | **Timeframe :** 1h

**Logique :**
- LONG : MACD histogram croise de negatif a positif + amplitude > 0.5 x ATR + RSI en zone neutre (40-60)
- SHORT : inverse

**Concept :** Le croisement zero du MACD signal le debut d'un mouvement. Le filtre RSI neutre assure qu'on entre en **debut** de mouvement (pas deja en surachat/survente). L'amplitude filtre les faux signaux faibles.

**Exits :** Trailing stop 1.5x ATR, TP 3x ATR. Le SL suit le meilleur prix atteint.

**Parametres :** Levier 5x, check 10s, cooldown 120s, max hold 4h

---

### 3. rsi_divergence_1h.lua — RSI Divergence

**Type :** Divergence | **Timeframe :** 1h

**Logique :**
- BULLISH DIV : prix fait un **lower low** mais RSI fait un **higher low** → le selling pressure diminue
- BEARISH DIV : prix fait un **higher high** mais RSI fait un **lower high** → le buying pressure diminue

**Implementation :**
- Scan des candles historiques (20 barres) via `bot.get_candles()`
- Detection des swing pivots (fenetre de 2 barres)
- Calcul RSI(14) interne sur les closes des candles
- Correspondance RSI/prix avec tolerance d'index de 2 barres
- Signal valide seulement si le pivot le plus recent est dans les 3 dernieres barres

**Parametres :** SL 1.5%, TP 3%, levier 5x, check 15s, cooldown 5min, max hold 4h

---

### 4. ema_adx_trend_4h.lua — EMA + ADX Trend

**Type :** Trend Following | **Timeframe :** 4h

**Logique :**
- LONG : EMA(12) > EMA(26) + ADX > 25 + prix > SMA(200)
- SHORT : EMA(12) < EMA(26) + ADX > 25 + prix < SMA(200)
- Exit 1 : croisement EMA inverse
- Exit 2 : ADX descend sous 20 (tendance s'affaiblit)

**Concept :** L'ADX confirme la force de la tendance (>25 = tendance forte). Le SMA(200) filtre la direction macro. Les deux conditions d'exit protegent contre les retournements.

**Parametres :** SL 2.5%, TP 6%, levier 3x, check 30s, cooldown 10min, max hold 24h

---

### 5. bb_kc_squeeze_1h.lua — BB/KC Squeeze Breakout

**Type :** Breakout (volatilite) | **Timeframe :** 1h

**Logique :**
- **Detection squeeze** : BB s'enferme a l'interieur des Keltner Channels (compression de volatilite)
- **Breakout** : quand le squeeze se relache (BB sort des KC), entree dans la direction du MACD
  - MACD histogram > 0 → LONG
  - MACD histogram < 0 → SHORT

**Concept :** La compression BB < KC precede statistiquement les mouvements explosifs. Le MACD donne la direction. C'est le "TTM Squeeze" popularise par John Carter.

**Exits :** SL 2x ATR, TP 3x ATR

**Parametres :** Levier 3x, check 15s, cooldown 120s, max hold 4h

---

### 6. donchian_breakout_1d.lua — Donchian Breakout

**Type :** Breakout/Trend | **Timeframe :** 1d (daily)

**Logique :**
- LONG seulement : prix > Donchian(20) upper (plus haut des 20 derniers jours) + prix > SMA(200)
- Exit trailing : quand le prix descend sous le Donchian(10) lower (plus bas des 10 derniers jours)

**Concept :** Systeme classique de Turtle Trading. Le breakout du canal 20j initie le trade, le canal 10j sert de stop suiveur. Le SMA(200) filtre les faux breakouts en bear market.

**Implementation :** Calcul manuel du Donchian(10) via `bot.get_candles()` pour l'exit trailing.

**Parametres :** SL 2x ATR, levier 2x (faible pour daily), check 60s, cooldown 1h, max hold 7j

---

### 7. vwap_reversion_15m.lua — VWAP Mean Reversion

**Type :** Mean Reversion | **Timeframe :** 15m

**Logique :**
- LONG : prix < VWAP - 2 x ATR + RSI < 35 (survente extreme sous le VWAP)
- SHORT : prix > VWAP + 2 x ATR + RSI > 65 (surachat extreme au-dessus du VWAP)
- Target : retour au VWAP (exit dynamique)

**Concept :** Le VWAP est la "fair value" institutionnelle. Des ecarts de 2x ATR representent des deviations statistiquement extremes qui tendent a mean-reverter. Le RSI confirme la survente/surachat.

**Exits :** Exit dynamique quand le prix revient au VWAP. Backup: SL 1%, TP cap 2%.

**Parametres :** Levier 5x, check 5s, cooldown 60s, max hold 1h

---

### 8. stochrsi_scalp_5m.lua — StochRSI + CCI Scalping

**Type :** Scalping | **Timeframe :** 5m

**Logique :**
- LONG : StochRSI K < 20 + CCI < -100 + prix > SMA(200)
- SHORT : StochRSI K > 80 + CCI > 100 + prix < SMA(200)
- Exit dynamique : StochRSI atteint 50 OU CCI atteint 0

**Concept :** Double confirmation d'oversold/overbought (StochRSI + CCI). Le SMA(200) filtre la tendance pour eviter les entries contre-tendance. Les exits dynamiques capturent la mean reversion avant le TP fixe.

**Parametres :** SL 0.8%, TP 1.5%, levier 5x, check 3s, cooldown 30s, max hold 30min

---

### 9. williams_obv_4h.lua — Williams %R + OBV Swing

**Type :** Swing Trading | **Timeframe :** 4h

**Logique :**
- LONG : Williams %R < -80 (oversold) + OBV > OBV SMA 20 (volume confirme) + prix > SMA(50)
- SHORT : Williams %R > -20 (overbought) + OBV < OBV SMA + prix < SMA(50)
- Exit LONG : %R atteint -50 OU OBV passe sous sa SMA (divergence volume)

**Concept :** Williams %R identifie les extremes, l'OBV confirme que le volume soutient le mouvement. Si l'OBV diverge (prix monte mais OBV baisse), c'est un signal de sortie precoce.

**Parametres :** SL 2%, TP 5%, levier 3x, check 30s, cooldown 10min, max hold 8h

---

### 10. regime_adaptive_1h.lua — Regime Adaptatif

**Type :** Adaptatif (2 modes) | **Timeframe :** 1h

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

**Concept :** S'adapte dynamiquement au regime de marche. Evite d'appliquer du mean reversion en marche tendanciel et inversement. La detection de regime est la cle de la performance.

**Parametres :** Levier 5x, check 10s, cooldown 120s, max hold 4h

---

### 11. elder_mtf_15m.lua — Elder Triple Screen

**Type :** Multi-Timeframe | **Timeframe :** 15m (primaire)

**3 ecrans (Elder) :**
1. **Screen 1 — Tendance 4h** (proxy via MAs longues) : EMA(26) > SMA(200) = haussier
2. **Screen 2 — Pullback 1h** (proxy via RSI) : RSI < 40 en tendance haussiere = zone d'achat
3. **Screen 3 — Declencheur 15m** : MACD histogram croise au-dessus de 0

**Logique :**
- LONG : tendance haussiere + pullback RSI + MACD cross up
- SHORT : tendance baissiere + RSI > 60 + MACD cross down
- Exit : retournement de tendance confirme (macro inverse + MACD confirme)

**Concept :** Systeme classique d'Alexander Elder. Aligne 3 timeframes pour filtrer les signaux. Le tracking de `prev_macd_hist` detecte les croisements zero.

**Parametres :** SL 2%, TP 4%, levier 5x, check 5s, cooldown 120s, max hold 4h

---

### 12. ichimoku_trend_4h.lua — Ichimoku Cloud Trend

**Type :** Trend Following (Ichimoku) | **Timeframe :** 4h

**Logique :**
- LONG : prix au-dessus du nuage + Tenkan > Kijun + RSI > 50 + ADX > 20
- SHORT : prix en-dessous du nuage + Tenkan < Kijun + RSI < 50 + ADX > 20
- **Trailing stop** : le SL suit le Tenkan-sen (buffer -0.3%), ne monte jamais vers le bas
- **Exit d'urgence** : fermeture si le prix entre dans le nuage contre la position

**Concept :** Le nuage Ichimoku donne support/resistance dynamiques et direction de tendance. Le Tenkan/Kijun cross est le signal d'entree. Le trailing sous le Tenkan protege les gains tout en laissant courir les tendances.

**Parametres :** SL 3%, TP 8%, levier 3x, trail buffer 0.3%, check 30s, cooldown 10min, max hold 12h

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
| **ADX** | `adx`, `plus_di`, `minus_di` | 14, Wilder |
| **Keltner Channels** | `kc_upper`, `kc_middle`, `kc_lower` | EMA 20, ATR 10, 1.5x |
| **Donchian Channels** | `dc_upper`, `dc_lower`, `dc_middle` | 20 |
| **Stochastic RSI** | `stoch_rsi_k`, `stoch_rsi_d` | RSI 14, Stoch 14, K=3, D=3 |
| **CCI** | `cci` | 20 |
| **Williams %R** | `williams_r` | 14 |
| **OBV** | `obv`, `obv_sma` | cumul + SMA 20 |
| **Ichimoku** | `ichi_tenkan`, `ichi_kijun`, `ichi_senkou_a`, `ichi_senkou_b`, `ichi_chikou` | 9, 26, 52 |

**Signaux derives :** `above_sma200`, `golden_cross`, `rsi_oversold`, `rsi_overbought`, `bb_squeeze`, `macd_bullish`, `adx_trending`, `kc_squeeze`, `ichi_bullish`

---

## Backtest

```bash
# Une seule strategie
./build/backtest_json strategies/triple_confirm_15m.lua ETH 0 365 15m

# Toutes les strategies (batch)
./tools/backtest_all.sh 365 0
```

Le runner effectue un walk-forward 60% IS / 40% OOS avec verdicts automatiques :
- **DEPLOYABLE** : Sharpe OOS >= 1.5, PF >= 1.5, alpha > 0, DD < 20%
- **A_OPTIMISER** : Sharpe >= 0.8, PF >= 1.2
- **MARGINAL** : profitable mais metriques faibles
- **INSUFFISANT** : < 30 trades
- **ABANDON** : perte, DD > 50%, ou Sharpe < 0.5
