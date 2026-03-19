# Trading Bot

An algorithmic trading bot connected to [Hyperliquid](https://hyperliquid.xyz), written in C for performance with Lua-based strategies for flexibility.

## Features

- **Hyperliquid connector** — REST + WebSocket, EIP-712 signing, limit/trigger/IOC orders
- **Lua strategies** — sandboxed, hot-reload (5s), multi-coin per file, trailing stop, ATR-adaptive sizing, drawdown guard
- **Risk management** — daily loss limit, emergency close, circuit breaker, velocity guard, losing streak pause
- **Backtesting** — multi-timeframe 5m simulation, walk-forward IS/OOS validation, Sharpe/Sortino/drawdown metrics
- **Desktop GUI** — Electron + React (dashboard, market overview, strategy browser, interactive backtesting, settings)
- **Paper trading** — real market data, simulated execution, per-strategy paper/live mode (mixed mode)
- **18+ technical indicators** — SMA, EMA, RSI, MACD, BB, ATR, VWAP, ADX, Keltner, Ichimoku, CMF, MFI, Squeeze Momentum, and more
- **Funding rate integration** — historical Binance funding rates in backtest + analysis (12 FR signals)
- **Monte Carlo simulation** — bootstrap risk estimation integrated in backtest output (P(ruin), P95 DD, worst-case scenarios)

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Tests

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

6 test suites, 128 tests.

## Configuration

### Environment Variables

```bash
# Required in LIVE mode (optional in paper trading)
TB_PRIVATE_KEY=0x...        # Hyperliquid API Wallet private key
TB_WALLET_ADDRESS=0x...     # Main wallet address
```

Store credentials in `.env` (auto-loaded by `scripts/start.sh`):

```bash
cp .env.template .env
chmod 600 .env
```

### Config File

`config/bot_config.json` — exchange URLs, risk parameters, active strategies with coins, paper mode. See the file for full options.

Paper trading can be configured globally or per-strategy:

```json
{
  "mode": { "paper_trading": false },
  "active_strategies": [
    {"file": "btc_sniper_1h.lua", "coins": ["BTC"]},
    {"file": "new_strategy.lua", "coins": ["ETH"], "paper_mode": true, "paper_balance": 500}
  ]
}
```

When `paper_mode` is set on a strategy, it overrides the global flag. Each paper strategy gets an isolated simulated exchange (own balance, positions, orders). Strategies without `paper_mode` inherit the global setting.

## License Activation

The GUI requires a license to run. Each license is tied to a single machine.

```bash
# 1. Generate admin keys (once)
node scripts/license_admin.js generate-keys

# 2. Copy the public key PEM into gui/electron/license.js (PUBLIC_KEY_PEM)

# 3. Generate license codes
node scripts/license_admin.js generate-codes 10

# 4. User launches the GUI → "Activation requise" screen shows their machine ID

# 5. Admin signs a token for that machine
node scripts/license_admin.js sign --code <UUID> --machine <machine-id>

# 6. User pastes the token → activated permanently

# List all codes and their status
node scripts/license_admin.js list
```

## Usage

```bash
# Foreground (with terminal dashboard)
./scripts/start.sh --foreground

# Background
./scripts/start.sh

# Stop
./scripts/stop.sh

# GUI
cd gui && npm install && npm run dev
```

## Strategies actives

| Asset | Fichier | Statut | Note /10 |
|-------|---------|--------|----------|
| BTC | `btc_sniper_1h.lua` | LIVE | 6.5 |
| DOGE | `doge_sniper_relaxed_1h.lua` | LIVE | 7.5 |
| SOL | `sol_range_breakout_1h.lua` | LIVE | 5.5 |
| ETH | — | RECHERCHE | — |

## Backtesting

Backtests use **5m candle simulation** with indicator aggregation to the strategy's native timeframe. This prevents the same-candle fill bug (entry + SL/TP filling on the same bar) and produces realistic results.

```bash
# Download candle data (required before first backtest)
./build/candle_fetcher --coins BTC,ETH,SOL,DOGE --intervals 5m --days 3200

# Download funding rates (optional, enables FR signals in backtest + analysis)
./build/funding_fetcher --coins BTC,ETH,SOL,DOGE --days 3200

# Single strategy (last arg = strategy TF, simulation always uses 5m)
./build/backtest_json strategies/btc_sniper_1h.lua BTC 0 90 1h

# All strategies batch
./scripts/backtest_all.sh 365 0

# Full report
./scripts/backtest_full_report.sh
```

### Signal Scanner (C)

```bash
# C-native signal scanner — 65 signals, combos 1-3, TP/SL grid, walk-forward IS/OOS
./build/signal_scanner DOGE 2000 both 2      # coin, days, direction, max_combo
./build/signal_scanner BTC 2000 long 3
./build/signal_scanner ETH 2000 both 2
```

### Grid Search TP/SL

```bash
# Grid search TP/SL for a specific strategy (loops TP 1.0→6.0 × SL 0.5→4.0)
./scripts/grid_search.sh strategies/doge_sniper_relaxed_1h.lua DOGE 2000 1h
```

### Regime Analyzer (C)

```bash
# Per-regime signal analysis with walk-forward validation and Monte Carlo
./build/regime_analyzer ETH 2000 1h --validate --montecarlo
```

See [docs/strategies.md](docs/strategies.md) for strategy details and [docs/backtest-market-periods.md](docs/backtest-market-periods.md) for results.

## Dependencies

libcurl, OpenSSL, libwebsockets, Lua 5.4, libsecp256k1, msgpack-c, SQLite3 (all via Homebrew on macOS). yyjson fetched by CMake.

## Documentation

| Document | Description |
|---|---|
| [docs/strategies.md](docs/strategies.md) | Strategy description and parameters |
| [docs/backtest-market-periods.md](docs/backtest-market-periods.md) | Multi-period backtest results (5m simulation) |
| [docs/pentest.md](docs/pentest.md) | Security audit report |
| [docs/marcus_venn_audit.md](docs/marcus_venn_audit.md) | Trading audit & $500 deployment plan |
| [CLAUDE.md](CLAUDE.md) | AI-friendly project map (architecture, conventions, key files) |

## License

Personal use only.
