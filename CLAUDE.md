# Trading Bot — AI Context

Algorithmic trading bot for Hyperliquid (perps). Engine in C (performance), strategies in Lua (flexibility), GUI in Electron+React. Supports paper and live trading, 12 strategies, multi-coin per file, hot-reload, compound sizing (10% equity/trade).

## Directory Structure

```
src/                    C engine source
  main.c               Entry point
  core/                 engine.c, config.c, decimal.c, db.c, logging.c, types.h
  exchange/             hl_rest.c, hl_ws.c, hl_signing.c, order_manager.c, paper_exchange.c
  strategy/             lua_engine.c, strategy_api.c, indicators.c
  data/                 macro_fetcher.c, twitter_sentiment.c, fear_greed.c, data_manager.c
  risk/                 risk_manager.c
  report/               dashboard.c, report_gen.c
  backtest/             backtest_engine.c
strategies/             12 Lua strategies + template
config/                 bot_config.json (no secrets — secrets in .env)
gui/                    Electron + React desktop app
  electron/             main.js, preload.js, ipc/ (bot, config, strategies, backtest, db, logs, ws, market, sync)
  src/                  App.jsx, pages/ (Dashboard, Market, Strategies, Backtest, Settings), components/, hooks/
tests/                  Unit tests + benchmarks (test_*.c, bench_*.c, backtest_*.c)
scripts/                start.sh, stop.sh, backtest_all.sh, backtest_full_report.sh, backtest_multi_period.sh
tools/                  candle_fetcher.c (historical data downloader with Binance fallback)
docs/                   strategies.md, backtest-report.md, backtest-results.md, pentest.md
data/                   SQLite DBs (trading_bot.db, candle_cache.db), backtest results
logs/                   Runtime logs
```

## Build & Run

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)

# Debug with sanitizers
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DTB_SANITIZERS=ON && cmake --build build -j$(nproc)

# Tests (6 suites, 128 tests)
cmake --build build && ctest --test-dir build --output-on-failure

# Run
./scripts/start.sh --foreground   # or ./build/trading_bot

# GUI
cd gui && npm install && npm run dev

# Backtest
./build/backtest_json strategies/bb_scalp_15m.lua ETH 0 30 15m
./scripts/backtest_all.sh 365 0
```

## Key Files by Domain

**Engine lifecycle**: `src/core/engine.c` (create/start/stop/destroy, wires all subsystems)
**Config**: `src/core/config.c` + `config/bot_config.json` + `.env` (secrets)
**Orders**: `src/exchange/order_manager.c` (ALO entries, trigger SL/TP, asset resolution)
**Signing**: `src/exchange/hl_signing.c` (EIP-712, secp256k1+keccak256)
**WebSocket**: `src/exchange/hl_ws.c` (allMids, l2Book, candles, fills)
**Strategies**: `src/strategy/lua_engine.c` (sandbox, hot-reload, COIN injection)
**Strategy API**: `src/strategy/strategy_api.c` (18 bot.* functions for Lua)
**Indicators**: `src/strategy/indicators.c` (SMA, EMA, RSI, MACD, BB, ATR, VWAP, ADX, Keltner, Ichimoku, etc.)
**Risk**: `src/risk/risk_manager.c` (daily loss, circuit breaker, leverage, position limits)
**Backtest**: `src/backtest/backtest_engine.c`
**GUI root**: `gui/src/App.jsx` (hoisted state: marketData, botStatus, tradeNotifications)
**GUI IPC**: `gui/electron/ipc/` (9 namespaces)

## Configuration

- **Secrets**: `.env` file (TB_PRIVATE_KEY, TB_WALLET_ADDRESS, TB_MACRO_API_KEY) — never hardcoded
- **Config**: `config/bot_config.json` — exchange URLs, risk params, active strategies+coins, paper mode
- **Multi-coin**: `"coins": ["ETH","BTC","SOL","DOGE"]` in strategy config, engine injects `COIN` global
- **Risk (%-based)**: daily_loss_pct=8, emergency_close_pct=6, max_leverage=5, max_position_pct=200

## Conventions

### C to Lua Indicator Fields
`rsi`, `bb_upper`, `bb_middle`, `bb_lower`, `bb_width`, `bb_squeeze`, `sma_20`, `sma_50`, `sma_200`, `ema_12`, `ema_26`, `macd`, `macd_signal`, `macd_histogram`, `atr`, `vwap`, `adx_14`, `plus_di`, `minus_di`, `kc_upper/middle/lower`, `dc_upper/middle/lower`, `stoch_rsi_k/d`, `cci_20`, `williams_r`, `obv`, `obv_sma`, `ichi_tenkan/kijun/senkou_a/senkou_b/chikou`

### Lua Strategy Callbacks
`on_init(config)`, `on_tick(data)`, `on_fill(fill)`, `on_timer()`, `on_book(book)`, `on_shutdown()`

### Hyperliquid Fees
Maker: 0.0150%, Taker: 0.0450%. ALO entries (maker), trigger exits (taker). Round trip = 0.06%.

### Backtest Aliases (compatible with live)
`sma`=sma_20, `ema`/`ema_fast`=ema_12, `ema_slow`=ema_26, `bb_mid`=bb_middle

### Critical Gotchas
- MACD needs 34+ candles: use `get_indicators(coin, tf, 50, mid)` minimum
- `ind.valid` does NOT exist: test `if not ind then return end`
- `bb_middle` not `bb_mid` from C (backtest has alias)
- `get_indicators` 4th arg = live_price (injects into last candle close)

## Dependencies (macOS)
libcurl, OpenSSL, libwebsockets, Lua 5.4, libsecp256k1, msgpack-c, SQLite3 (all via Homebrew). yyjson via CMake FetchContent.

## Active Strategies (production — $500 plan)
- `regime_adaptive_1h.lua` — ADX regime detection, ATR SL/TP (primary, 10% equity, max_size $80)
- `bb_scalp_15m.lua` — BB mean reversion, ATR SL, graduated TP (secondary, 8% equity)
- `ichimoku_trend_4h.lua` — Ichimoku cloud trend, ATR SL 2.0x [2.0-4.5%] (secondary, 5% equity)
- Coins: BTC, ETH, SOL (= 9 instances)
- Risk: daily_loss 6%, emergency 5%, max_leverage 5x, max_position 150%, global_exposure 300%

## Rules
- NEVER run git commands (user-managed)
- Keep security best practices (OWASP, no hardcoded secrets)
- Language: French for communication
