# Trading Bot

An algorithmic trading bot connected to [Hyperliquid](https://hyperliquid.xyz), written in C for performance with Lua-based strategies for flexibility.

## Features

- **Hyperliquid connector** — REST + WebSocket, EIP-712 signing, limit/trigger/IOC orders
- **12 Lua strategies** — sandboxed, hot-reload (5s), multi-coin per file, compound sizing (10% equity/trade)
- **Risk management** — daily loss limit, emergency close, circuit breaker, velocity guard, losing streak pause
- **Backtesting** — walk-forward IS/OOS validation, Sharpe/Sortino/drawdown metrics
- **Desktop GUI** — Electron + React (dashboard, market overview, strategy browser, interactive backtesting, settings)
- **Paper trading** — real market data, simulated execution
- **15+ technical indicators** — SMA, EMA, RSI, MACD, BB, ATR, VWAP, ADX, Keltner, Ichimoku, and more

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

# Optional
TB_MACRO_API_KEY=...        # FinancialModelingPrep (indices, stocks, commodities)
```

Store credentials in `.env` (auto-loaded by `scripts/start.sh`):

```bash
cp .env.template .env
chmod 600 .env
```

### Config File

`config/bot_config.json` — exchange URLs, risk parameters, active strategies with coins, paper mode. See the file for full options.

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

## Backtesting

```bash
# Single strategy
./build/backtest_json strategies/bb_scalp_15m.lua ETH 0 30 15m

# All strategies batch
./scripts/backtest_all.sh 365 0

# Full report
./scripts/backtest_full_report.sh
```

See [docs/strategies.md](docs/strategies.md) for strategy details and [docs/backtest-report.md](docs/backtest-report.md) for results.

## Dependencies

libcurl, OpenSSL, libwebsockets, Lua 5.4, libsecp256k1, msgpack-c, SQLite3 (all via Homebrew on macOS). yyjson fetched by CMake.

## Documentation

| Document | Description |
|---|---|
| [docs/strategies.md](docs/strategies.md) | Strategy descriptions, parameters, and usage |
| [docs/backtest-report.md](docs/backtest-report.md) | Full backtest report with OOS results |
| [docs/backtest-results.md](docs/backtest-results.md) | Backtest results summary |
| [docs/pentest.md](docs/pentest.md) | Security audit report |
| [docs/marcus_venn_audit.md](docs/marcus_venn_audit.md) | Trading audit & $500 deployment plan |
| [CLAUDE.md](CLAUDE.md) | AI-friendly project map (architecture, conventions, key files) |

## License

Personal use only.
