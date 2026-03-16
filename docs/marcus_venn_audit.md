# Audit Trading Bot — Marcus Venn (2026-03-12)

Audit complet du bot de trading algorithmique Hyperliquid par Marcus Venn,
trader professionnel 20 ans d'experience (equities, futures, crypto).

## Verdict general

Architecture technique de qualite professionnelle (C + Lua sandboxe, circuit breaker,
risk manager multi-couche, ALO entries). Le code est pret. Le probleme initial etait
le capital ($104 pour 12 instances simultanees).

## Plan deploiement $500

### Phase 1 — Paper trading (2 semaines obligatoires)

Config exacte identique au live prevu. Valider les metriques avant de passer en live.

### Phase 2 — Go-live progressif (semaines 3-4)

Conditions de passage live :
- Win rate > 40% regime_adaptive, > 35% ichimoku, > 45% bb_scalp
- Sharpe > 0.5 annualise
- Max drawdown paper < 10%

Semaine 3 : Live $250 (50%). Semaine 4 : Si metriques tiennent, monter a $500.

### Phase 3 — Monitoring production (semaine 5+)

| Metrique | Seuil alerte |
|----------|-------------|
| P&L journalier | < -4% (alerte jaune) |
| P&L hebdo | < -8% (alerte rouge, review) |
| Win rate 7j par strat | < 30% (desactiver) |
| Fee drag / P&L brut | > 25% |
| Trades/jour | > 15 (over-trading) |
| Max drawdown total | > 15% (stop tout, audit) |

## Strategies selectionnees (3 sur 12)

### 1. regime_adaptive_1h (PRIMARY) — Note: 7/10

- Hysteresis ADX (22/28) solide
- ATR-based SL/TP adaptatif
- Regime-change exit intelligent
- 10% equity, max_size $80

### NOTE (2026-03-16)

bb_scalp_15m, ichimoku_trend_4h et toutes les autres strategies secondaires ont ete supprimees apres backtest 5m multi-periodes. Seule regime_adaptive_1h est deployee. Voir `docs/backtest-market-periods.md`.

## Coins : 3 (BTC, ETH, SOL)

DOGE retire : spreads plus larges en %, volatilite 2-3x BTC,
SL ATR disproportionnes. 9 instances au lieu de 12.

## Risk parameters

| Parametre | Avant | Apres | Raison |
|-----------|-------|-------|--------|
| daily_loss_pct | 8% | 6% | $30/jour max, psychologiquement tenable |
| emergency_close_pct | 6% | 5% | Declenche avant le daily loss |
| max_leverage | 10x | 5x | Reduit risque de wipeout |
| max_position_pct | 200% | 150% | Cap par position |
| global_exposure | aucun | 300% | **NOUVEAU** — cap total notionnel toutes positions |

## Ameliorations implementees

### Global exposure cap (risk_manager.c)

- Somme du notionnel de TOUTES les positions ouvertes
- + notionnel du nouvel ordre
- Doit etre < 300% du account_value
- Nouveau code rejet: `TB_RISK_REJECT_GLOBAL_EXPOSURE`
- Position tracker lie au risk manager via `tb_risk_set_position_tracker()`

### Ichimoku SL ATR-based

- `sl_pct = 3.0` fixe remplace par `sl_atr_mult = 2.0` clampe [2.0%, 4.5%]
- Fallback 2.5% si ATR indisponible
- ATR 4h stocke a chaque tick, utilise au fill

### bb_scalp equity reduit

- `equity_pct` 0.10 → 0.08 (scalp = plus de trades, taille reduite)

### regime_adaptive max_size augmente

- `max_size` 60 → 80 (taille naturelle ~$50, cap de securite avec marge)

## Strategies a surveiller pour $1000+

Toutes les strategies secondaires ont ete testees en backtest 5m multi-periodes (2026-03-16) et se sont averees sous-performantes. Aucune n'a obtenu de verdict DEPLOYABLE. Voir `docs/backtest-market-periods.md` pour les resultats detailles.

## Pieges rappeles

1. **Overfitting backtest** : Ne pas re-optimiser les seuils avant 3 mois de data live
2. **Correlation** : BTC/ETH/SOL 0.7-0.9 en stress → si correlation 30j > 0.92, passer a 2 coins
3. **ALO partiels** : En backtest tout fill, en live ALO peut expirer → profil different
4. **Slippage trigger** : SL/TP a 2% de marge, en flash crash ca peut depasser
5. **Recalibration** : Mensuelle max, comparer live vs backtest, divergence > 20% = investiguer

## Recalibration mensuelle

1. Export metriques : Win rate, avg win/loss, Sharpe, max DD, fee drag (par strat/coin)
2. Comparer au backtest (divergence > 20% = investiguer)
3. Check correlations (> 0.92 = reduire a 2 coins)
4. Ne PAS re-optimiser les seuils (risque de curve-fitting le live)

---

*"Le code est pret. Le capital ne l'est pas encore."* — A $500 avec 3 strategies et 3 coins,
l'objectif est de prouver l'edge, pas de faire 50%/mois. Si apres 60 jours +5% net de frais
avec Sharpe > 0.5, tu as quelque chose. Et la tu peux scaler.
