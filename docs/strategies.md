# Strategies de Trading — Documentation

> Projet educatif par **tmege**. 3 strategies paper : BTC, DOGE, SOL.
> ETH : aucune strategie viable — en cours de recherche.
> Moteur C avec indicateurs natifs, Lua sandboxe, multi-coin automatique.

---

## Portfolio actif

| Asset | Fichier | Statut | Note /10 |
|-------|---------|--------|----------|
| BTC | `btc_sniper_1h.lua` | PAPER | 6.5 — explosif mais rare |
| DOGE | `doge_sniper_relaxed_1h.lua` | PAPER | 7.5 — le plus robuste |
| SOL | `sol_range_breakout_1h.lua` | PAPER | 5.5 — volatile, bear supplement aide |
| ETH | — | RECHERCHE | 1.0 — strategie morte (ATR<0.4% jamais atteint) |

---

## Architecture commune

Chaque strategie suit le meme pattern :

- **COIN global** injecte par le moteur C avant chargement
- **Instance name** : `{strategie}_{coin}` (ex: `sniper_1h_btc`)
- **5 callbacks** : `on_init`, `on_tick`, `on_fill`, `on_timer`, `on_shutdown`
- **Entry ALO** : maker entry avec offset 0.02% (`price * 0.9998` buy, `* 1.0002` sell)
- **SL/TP** : trigger orders reduce-only sur l'exchange
- **Etat persistant** : `trade_count`, `win_count`, `enabled` via `save_state`/`load_state`
- **Position recovery** : detecte les positions existantes au redemarrage
- **Max hold time** : fermeture forcee si la position est tenue trop longtemps (48h)
- **Drawdown guard** : sizing reduit ou arrete selon DD (daily/weekly/monthly/total)
- **Orphaned position guard** : ferme automatiquement les positions non trackees
- **Partial fill handling** : detection via `closed_pnl == 0` (entry) vs `!= 0` (exit)

---

## btc_sniper_1h.lua — BTC Sniper High-Conviction

**Type :** Ultra-selectif | **Timeframe :** 1h | **Coins :** BTC uniquement

**Principe :** Signaux rares a haute conviction, levier eleve (x7), 90% equity par trade.
~1.2 trades/mois, maximum conviction, sizing agressif.

**Signaux BTC (2) :**
- **L1** : RSI>65 + ATR<0.4% + MACD decelerant (2 bars) → TP 2.0%/SL 2.0%
- **S1** : RSI<30 + MACD<0 + ATR<0.4% → TP 2.0%/SL 2.0%

**Parametres :** Levier 7x, equity 90%, cooldown 4h, max_hold 48h, max_size $9999

**Backtest 90j multi-periodes :**
| Periode | Trades | WR | Return | Sharpe |
|---------|--------|----|--------|--------|
| Recent 90j | 2 | 100% | +25.7% | 35.55 |
| -90 a -180j | 5 | 100% | +77.0% | 24.89 |
| -180 a -270j | 11 | 63.6% | +24.9% | 1.73 |
| -270 a -360j | 5 | 40.0% | -18.3% | -2.59 |

**Forces :** WR tres eleve quand il trade (souvent 100%)
**Faiblesses :** Frequence faible (2-5 trades/90j), variance haute, 1 fenetre negative

---

## doge_sniper_relaxed_1h.lua — DOGE Sniper Relaxed

**Type :** Low-vol sniper RR 3:1 | **Timeframe :** 1h | **Coins :** DOGE

**Principe :** Filtre low-vol (ATR<0.9%) + RSI extreme + RR fixe 3:1.
S1 est le signal dominant (DOGE 82% en bear regime, 157/291 trades).

**Signaux DOGE (4) :**
- **L1** : RSI>65 + ATR<0.9% + MACD decelerant → TP 4.5%/SL 1.5%
- **L3** : RSI>65 + ADX>20 + ATR<0.9% → TP 4.5%/SL 1.5%
- **S1** : RSI<35 + MACD<0 + ATR<0.9% → TP 4.5%/SL 1.5% (dominant)
- **S2** : RSI<35 + ATR<0.9% + (SMA20<SMA50 ou MACD<0) → TP 4.5%/SL 1.5%

**Parametres :** Levier 5x, equity 50%, cooldown 4h, max_hold 48h

**Backtest 90j multi-periodes :**
| Periode | Trades | WR | Return | Sharpe |
|---------|--------|----|--------|--------|
| Recent 90j | 19 | 47.4% | +15.0% | 1.77 |
| -90 a -180j | 8 | 37.5% | +5.3% | 0.77 |
| -180 a -270j | 7 | 42.9% | +5.2% | 0.79 |
| -270 a -360j | 6 | 50.0% | +15.3% | 1.72 |
| -365 a -455j | 2 | 50.0% | +6.7% | 0.96 |
| -540 a -630j | 15 | 53.3% | +36.8% | 2.79 |

**Forces :** 6/6 fenetres positives, Sharpe moyen 1.47, DD max < 17%
**Faiblesses :** WR autour de 45% (compense par RR 3:1)

---

## sol_range_breakout_1h.lua — SOL Range Breakout + Bear Complement

**Type :** Range breakout + bear short | **Timeframe :** 1h | **Coins :** SOL
**Version :** 2.0.0 (merge RB + Bear B1)

**Principe :** Deux modes combines en single-position :
1. **Range Breakout** (tous regimes) : consolidation 24 bars + breakout volume
2. **Bear B1** (bear regime) : short momentum en contexte bear (prioritaire)

**Bear Regime Gate :** `close < SMA200 AND SMA50 < SMA200 AND ADX > 20`

**Signaux :**
- **RB** : range<4% sur 24 bars + vol×1.2 + breakout → TP 4.5%/SL 1.5% (long ou short)
- **B1** : RSI<35 + MACD<0 + ATR<1.2% + bear regime → TP 6.0%/SL 2.0% (short only)

**Parametres :** Levier 5x, equity 40%, cooldown 4h, max_hold 48h

**Backtest 90j multi-periodes :**
| Periode | Trades | WR | Return | Sharpe |
|---------|--------|----|--------|--------|
| Recent 90j | 25 | 44.0% | +47.5% | 2.80 |
| -90 a -180j | 21 | 38.1% | +30.5% | 2.04 |
| -180 a -270j | 26 | 26.9% | -6.3% | -0.33 |
| -270 a -360j | 21 | 28.6% | -6.6% | -0.64 |
| -365 a -455j | 14 | 71.4% | +107.8% | 6.20 |
| -540 a -630j | 19 | 26.3% | -14.3% | -1.70 |

**Forces :** Frequence elevee (21 trades/90j), tres bon recemment
**Faiblesses :** 3/6 fenetres negatives, DD peut monter a 25%

---

## ETH — Aucune strategie active

**Statut :** Strategie morte — `btc_sniper_1h.lua` produit 0 trade sur ETH (ATR<0.4% jamais atteint).

**Tentatives echouees :**
| Config | Sharpe 730j | Verdict | Raison |
|--------|-------------|---------|--------|
| sniper params BTC (ATR<0.4%) | — | NO-GO | ATR<0.4% = 3% du temps sur ETH |
| sniper_extended RR 3:1 | 0.36 | NO-GO | DD incompressible > 36% |
| sniper_extended RR 1.3 | 0.62 | WEAK | Sharpe trop faible |
| DOGE-logic ATR<1.4% | 1.21 (WF) | NO-GO | C backtest 730j negatif (-10.6%) |

**Ce qui a ete teste et rejete :**
- 15min indicateurs classiques → frais/ATR ratio fatal
- Monday Levels standalone → 3 trades/mois, trop rare
- EMA cross, Donchian, BB reversion, Supertrend, Z-score → tous negatifs apres frais
- Session open momentum → 6 signaux/mois insuffisant

**Conclusion :** Le filtre low_vol est l'edge principal. ETH a un ATR median trop eleve pour les seuils BTC/DOGE. Un seuil adapte (ATR<1.4%) montre un edge en Python WF mais disparait en backtest C.

---

## Indicateurs C disponibles

Tous les indicateurs sont calcules nativement en C et exposes via `bot.get_indicators(coin, interval, count, live_price)` :

| Indicateur | Champ Lua | Params |
|-----------|-----------|--------|
| SMA | `sma_20`, `sma_50`, `sma_200` | 20, 50, 200 |
| EMA | `ema_12`, `ema_26` | 12, 26 |
| RSI | `rsi` | 14 |
| MACD | `macd`, `macd_signal`, `macd_histogram` | 12, 26, 9 |
| Bollinger Bands | `bb_upper`, `bb_middle`, `bb_lower`, `bb_width`, `bb_squeeze` | 20, 2.0 |
| ATR | `atr` | 14 |
| VWAP | `vwap` | cumulatif |
| ADX | `adx`, `plus_di`, `minus_di` | 14, Wilder |
| Keltner Channels | `kc_upper`, `kc_middle`, `kc_lower` | EMA 20, ATR 10, 1.5x |
| Donchian Channels | `dc_upper`, `dc_lower`, `dc_middle` | 20 |
| Stochastic RSI | `stoch_rsi_k`, `stoch_rsi_d` | RSI 14, Stoch 14, K=3, D=3 |
| CCI | `cci_20` | 20 |
| Williams %R | `williams_r` | 14 |
| OBV | `obv`, `obv_sma` | cumul + SMA 20 |
| Ichimoku | `ichi_tenkan`, `ichi_kijun`, `ichi_senkou_a`, `ichi_senkou_b`, `ichi_chikou` | 9, 26, 52 |
| CMF | `cmf` | 20 |
| MFI | `mfi` | 14 |
| Squeeze Momentum | `squeeze_mom`, `squeeze_on` | BB 20,2 + KC 20,1.5 |
| ROC | `roc` | 12 |
| Z-Score | `zscore` | 20 |
| FVG | `fvg_bull`, `fvg_bear`, `fvg_size` | 3 bars |
| Supertrend | `supertrend`, `supertrend_up` | 10, 3.0 |
| Parabolic SAR | `psar`, `psar_up` | 0.02, 0.2 |

**Live-only API :**
- `bot.get_funding_rate(coin)` → `{rate, premium, mark_px}` (60s cache)
- `bot.get_open_interest(coin)` → `number` (60s cache)

---

## Backtest

```bash
# Une seule strategie
./build/backtest_json strategies/btc_sniper_1h.lua BTC 0 90 1h

# Toutes les strategies
./scripts/backtest_all.sh 365 0

# Full report
./scripts/backtest_full_report.sh
```

Le runner effectue un walk-forward 60% IS / 40% OOS avec verdicts automatiques.

---

## Regles fondamentales

1. **1 coin = 1 strategie** — enforce dans le moteur C, jamais de doublon
2. **Low_vol est l'edge principal** — pas les indicateurs eux-memes
3. **RR fixe 3:1 > SL dynamique** — pour la frequence et la robustesse
4. **La frequence vient du nombre d'assets** — pas du timeframe
5. **15min non viable** — frais = 30% de l'ATR
6. **Toujours valider en backtest C** — Python surestime systematiquement de 5-13pp WR
7. **Walk-forward 6 folds minimum** avant toute confiance dans une strategie
