# Trading Bot — AI Context

Algorithmic trading bot for Hyperliquid (perps). Engine in C (performance), strategies in Lua (flexibility), GUI in Electron+React. Supports paper and live trading, multi-coin per file, hot-reload, compound sizing.

## Directory Structure

```
src/                    C engine source
  main.c               Entry point
  core/                 engine.c, config.c, decimal.c, db.c, logging.c, types.h
  exchange/             hl_rest.c, hl_ws.c, hl_signing.c, order_manager.c, paper_exchange.c
  strategy/             lua_engine.c, strategy_api.c, indicators.c
  data/                 twitter_sentiment.c, fear_greed.c, data_manager.c
  risk/                 risk_manager.c
  report/               dashboard.c
  backtest/             backtest_engine.c
strategies/             btc_sniper_1h.lua (active) + strategy_template.lua (template)
config/                 bot_config.json (no secrets — secrets in .env)
gui/                    Electron + React desktop app
  electron/             main.js, preload.js, license.js, ipc/ (bot, config, strategies, backtest, db, logs, ws, market, sync, license)
  src/                  App.jsx, pages/ (Dashboard, Market, Strategies, Backtest, Settings, LicenseGate), components/, hooks/
tests/                  Unit tests + benchmarks (test_*.c, bench_*.c, backtest_*.c)
scripts/                start.sh, stop.sh, backtest_all.sh, backtest_full_report.sh, backtest_multi_period.sh, license_admin.js
tools/                  candle_fetcher.c, strategy_analyzer.py, regime_analyzer.py, analyzer/ (Python analysis pipeline)
docs/                   strategies.md, backtest-market-periods.md, pentest.md
data/                   SQLite DBs (trading_bot.db, candle_cache.db — 5m candles only), backtest results
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

# Download 5m candles (required before backtesting)
./build/candle_fetcher --coins BTC,ETH,SOL,DOGE --intervals 5m --days 3200

# Backtest (always uses 5m candles internally, last arg = strategy TF)
./build/backtest_json strategies/btc_sniper_1h.lua BTC 0 90 1h
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
**Indicators**: `src/strategy/indicators.c` (SMA, EMA, RSI, MACD, BB, ATR, VWAP, ADX, Keltner, Ichimoku, CMF, MFI, Squeeze Momentum, etc.)
**Risk**: `src/risk/risk_manager.c` (daily loss, circuit breaker, leverage, position limits)
**Backtest**: `src/backtest/backtest_engine.c` (multi-TF: 5m simulation + TF aggregation)
**GUI root**: `gui/src/App.jsx` (license gate → AppMain, hoisted state: marketData, botStatus, tradeNotifications)
**GUI IPC**: `gui/electron/ipc/` (10 namespaces)
**License**: `gui/electron/license.js` (Ed25519 verify, AES-256-GCM storage, machine fingerprint)
**License admin**: `scripts/license_admin.js` (generate-keys, generate-codes, sign, list)

## Configuration

- **Secrets**: `.env` file (TB_PRIVATE_KEY, TB_WALLET_ADDRESS) — never hardcoded
- **Config**: `config/bot_config.json` — exchange URLs, risk params, active strategies+coins, paper mode
- **Multi-coin**: `"coins": ["ETH","SOL"]` in strategy config, engine injects `COIN` global
- **Coin exclusivity**: each coin must belong to exactly one strategy — enforced at engine startup (rejects duplicates), GUI Settings (grays out taken coins), and Lua (no cross-strategy guards needed)
- **Paper mode**: global (`mode.paper_trading`) or per-strategy (`"paper_mode": true, "paper_balance": 500` in strategy config). Per-strategy paper creates an isolated `tb_paper_exchange_t` with its own balance/positions/orders. Mixed mode (some paper, some live) is supported. Lua global `PAPER_MODE` is injected per slot.
- **Risk (%-based)**: daily_loss_pct=6, emergency_close_pct=5, max_leverage=10, max_position_pct=700

## Conventions

### C to Lua Indicator Fields
`rsi`, `bb_upper`, `bb_middle`, `bb_lower`, `bb_width`, `bb_squeeze`, `sma_20`, `sma_50`, `sma_200`, `ema_12`, `ema_26`, `macd`, `macd_signal`, `macd_histogram`, `atr`, `vwap`, `adx_14`, `plus_di`, `minus_di`, `kc_upper/middle/lower`, `dc_upper/middle/lower`, `stoch_rsi_k/d`, `cci_20`, `williams_r`, `obv`, `obv_sma`, `ichi_tenkan/kijun/senkou_a/senkou_b/chikou`, `cmf`, `mfi`, `squeeze_mom`, `squeeze_on`, `roc`, `zscore`, `fvg_bull`, `fvg_bear`, `fvg_size`, `supertrend`, `supertrend_up`, `psar`, `psar_up`
Aliases (backtest-compatible): `sma`=sma_20, `ema`/`ema_fast`=ema_12, `ema_slow`=ema_26, `bb_mid`=bb_middle

### Lua Live-Only API
- `bot.get_funding_rate(coin)` → `{rate, premium, mark_px}` or `nil` (60s cache, not available in backtest)
- `bot.get_open_interest(coin)` → `number` or `nil` (60s cache, not available in backtest)

### Lua Strategy Callbacks
`on_init(config)`, `on_tick(data)`, `on_fill(fill)`, `on_timer()`, `on_book(book)`, `on_shutdown()`

### Hyperliquid Fees
Maker: 0.0150%, Taker: 0.0450%. ALO entries (maker), trigger exits (taker). Round trip = 0.06%.

### Backtest Multi-Timeframe (5m simulation)
- Backtests always load **5m candles** from SQLite cache (`data/candle_cache.db`)
- `backtest_engine.c` aggregates 5m candles into the strategy's native TF (1h, 4h...) via `aggregate_5m_into_tf()`
- `on_tick` is called only when a TF candle completes (e.g. every 12 ticks for 1h, 48 for 4h)
- Order fills are checked on 5m high/low — orders placed in same 5m candle are skipped (`placed_at_idx` guard)
- `get_indicators()` / `get_candles()` return aggregated TF candles (not raw 5m), with current 5m close injected as live_price
- `strategy_interval_ms` in `tb_backtest_config_t` controls the aggregation TF
- Candle data: only 5m stored in cache (15m/1h/4h/1d purged), up to 8+ years for BTC/ETH/SOL/DOGE via Binance

### Critical Gotchas
- MACD needs 34+ candles: use `get_indicators(coin, tf, 50, mid)` minimum
- `ind.valid` does NOT exist: test `if not ind then return end`
- `bb_middle` not `bb_mid` from C (backtest has alias)
- `get_indicators` 4th arg = live_price (injects into last candle close)
- **1 coin = 1 strategy**: never assign the same coin to multiple strategies (engine refuses to start)

## Dependencies (macOS)
libcurl, OpenSSL, libwebsockets, Lua 5.4, libsecp256k1, msgpack-c, SQLite3 (all via Homebrew). yyjson via CMake FetchContent.

## Strategies (production — $672 capital)
- **BTC**: `btc_sniper_1h.lua` — ultra-selective high-conviction (x7, 90% equity, 2 signals L1+S1, ATR<0.4%, TP/SL 2.0%/2.0%)
- **DOGE**: `doge_sniper_relaxed_1h.lua` — low-vol sniper RR 3:1 (x5, 50% equity, 4 signals L1+L3+S1+S2, ATR<0.9%, TP/SL 4.5%/1.5%)
- **SOL**: `sol_range_breakout_1h.lua` v2.0.0 — range breakout + bear complement (x5, 40% equity, RB TP/SL 4.5%/1.5%, B1 TP/SL 6.0%/2.0%)
- **ETH**: aucune strategie active — ATR<0.4% jamais atteint, en recherche
- **Template**: `strategy_template.lua` — skeleton for new strategies
- Coins: BTC, DOGE, SOL (1 coin = 1 strategy instance, exclusive)
- Risk: daily_loss 6%, emergency 5%, max_leverage 10x, max_position 700%
- **Mode: LIVE**

## Analysis Pipeline (Python)
- `tools/strategy_analyzer.py` — scan 63+ signaux × combos × grille TP/SL, rapports markdown
- `tools/regime_analyzer.py` — analyse per-régime + walk-forward + Monte Carlo
- `tools/analyzer/` — modules: data.py, indicators.py, labeling.py, signals.py, stats.py, regime.py, walk_forward.py, monte_carlo.py, report.py
- Données: bougies 5m agrégées depuis SQLite, 8+ ans ETH, 5+ ans SOL

## License System

Ed25519 asymmetric licensing — public key in app (verify only), private key admin-only (sign).

- **Flow**: User gets machineId from GUI → admin signs token → user activates
- **Machine ID**: SHA-256(hostname|cpuModel|platform|arch|MAC|totalMemGB) truncated 16 hex
- **Storage**: `license.dat` in `app.getPath('userData')`, AES-256-GCM encrypted with machineId-derived key
- **Token format**: base64url(JSON{c: code, m: machineId, s: Ed25519 signature of "code:machineId"})
- **1 code = 1 machine**: token is bound to machineId via signature
- **Admin CLI**: `scripts/license_admin.js` (generate-keys, generate-codes [n], sign --code --machine, list)
- **Codes DB**: `scripts/license_codes.txt` (gitignored)
- **Keys**: `scripts/license_private.pem` + `license_public.pem` (gitignored via *.pem)

## Rules
- NEVER run git commands (user-managed)
- Keep security best practices (OWASP, no hardcoded secrets)
- Language: French for communication
