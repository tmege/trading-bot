# Deploiement — Guide operationnel

## Procedure deploiement nouvelle strategie

1. **Backtest Python** — walk-forward 6 folds, gates definis (Sharpe > 0.8, WR > 38%, EV > 0)
2. **Backtest C** — toutes periodes (90j × 6 offsets), comparaison Python vs C (delta attendu: 5-13pp WR)
3. **Checklist robustesse 6/8** (voir ci-dessous)
4. **Test paper** — 1% equity minimum 1 semaine
5. **Montee progressive** — 10% → 25% → 50% → sizing cible

## Checklist robustesse (minimum 6/8)

| # | Check | Description |
|---|-------|-------------|
| 1 | pcall sur signaux | Signal checks proteges par pcall |
| 2 | Position existante guard | Ne pas re-enter si deja en position |
| 3 | Partial fills | Detection via closed_pnl == 0 (entry) / != 0 (exit) |
| 4 | Position orpheline | Guard qui ferme les positions non trackees |
| 5 | Prix validation | Offset ALO 0.02%, IOC fallback 1% |
| 6 | Liquidite min | Verification volume minimum avant entry |
| 7 | Logs timestampes | Chaque action loggee avec context |
| 8 | State persistence | save_state/load_state pour recovery |

## Seuils monitoring par asset

| Asset | Consec losses | Daily DD | Weekly DD | Monthly DD | Total DD |
|-------|---------------|----------|-----------|------------|----------|
| BTC | 3 | 10% | 15% | 20% | 20% |
| SOL | 3 | 8% | 18% | 25% | 34% |
| DOGE | 4 | 7% | 15% | 25% | 29% |

## Paliers drawdown en production

| DD | Action |
|----|--------|
| 7-8% | Sizing x 0.50 |
| 15-18% | Sizing x 0.25 |
| 25% | Sizing = 0 (stop) |
| 29-34% | Intervention humaine obligatoire |

## Commandes operationnelles

```bash
# Demarrer le bot (foreground)
./scripts/start.sh --foreground

# Demarrer en background
./scripts/start.sh

# Arreter proprement
./scripts/stop.sh

# GUI
cd gui && npm install && npm run dev

# Backtest rapide
./build/backtest_json strategies/btc_sniper_1h.lua BTC 0 90 1h

# Batch toutes strategies
./scripts/backtest_all.sh 365 0
```

## Variables d'environnement requises

```bash
# .env (ne JAMAIS committer)
TB_PRIVATE_KEY=0x...         # Cle privee wallet Hyperliquid
TB_WALLET_ADDRESS=0x...      # Adresse wallet principale
TB_ANTHROPIC_API_KEY=sk-...  # Pour AI Daily Digest (optionnel)
```

## Configuration production (bot_config.json)

```json
{
  "strategies": {
    "active": [
      {"file": "btc_sniper_1h.lua", "coins": ["BTC"]},
      {"file": "doge_sniper_relaxed_1h.lua", "coins": ["DOGE"]},
      {"file": "sol_range_breakout_1h.lua", "coins": ["SOL"]}
    ]
  },
  "risk": {
    "daily_loss_pct": 6,
    "emergency_close_pct": 5,
    "max_leverage": 10,
    "max_position_pct": 700
  },
  "mode": {"paper_trading": false}
}
```

## Regles absolues

1. **Jamais de modification Lua sans re-backtest** walk-forward 6 folds
2. **Jamais de secrets dans le code** — .env uniquement
3. **1 coin = 1 strategie** — le moteur refuse de demarrer si doublon
4. **Toujours valider en C** — Python surestime systematiquement
5. **Backup des positions** — save_state a chaque changement d'etat
