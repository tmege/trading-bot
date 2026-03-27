# Trading Bot — AI Context

Educational project by **tmege**. Algorithmic trading bot for Hyperliquid (perps). Engine in C (performance), strategies in Lua (flexibility), GUI in Electron+React. Paper trading only (educational build — live trading disabled), multi-coin per file, hot-reload, compound sizing.

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
strategies/             btc_sniper_1h.lua, doge_sniper_relaxed_1h.lua, sol_range_breakout_1h.lua (active) + strategy_template.lua
config/                 bot_config.json (no secrets — secrets in .env)
gui/                    Electron + React desktop app
  electron/             main.js, preload.js, license.js, ipc/ (bot, config, strategies, backtest, db, logs, ws, market, sync, license)
  src/                  App.jsx, pages/ (Dashboard, Market, Strategies, Backtest, Settings, LicenseGate), components/, hooks/
tests/                  Unit tests + benchmarks (test_*.c, bench_*.c, backtest_*.c)
scripts/                start.sh, stop.sh, backtest_all.sh, backtest_full_report.sh, backtest_multi_period.sh, grid_search.sh, license_admin.js
tools/                  candle_fetcher.c, funding_fetcher.c, signal_scanner.c, regime_analyzer.c (all C-native)
docs/                   strategies.md, backtest-market-periods.md, pentest.md, architecture_prompt.md
data/                   SQLite DBs (trading_bot.db, candle_cache.db — 5m candles + funding_rates), backtest results
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

# Download funding rates (optional, enables FR signals in backtest + analysis)
./build/funding_fetcher --coins BTC,ETH,SOL,DOGE --days 3200

# Backtest (always uses 5m candles internally, last arg = strategy TF)
./build/backtest_json strategies/btc_sniper_1h.lua BTC 0 90 1h
./build/backtest_json strategies/doge_sniper_relaxed_1h.lua DOGE 0 2000 1h 4.5 1.5  # with grid TP/SL override
./scripts/backtest_all.sh 365 0

# Grid search TP/SL (loops over TP×SL grid, outputs sorted by OOS Sharpe)
./scripts/grid_search.sh strategies/doge_sniper_relaxed_1h.lua DOGE 2000 1h

# Signal scanner (C-native, 65 signals × combos × TP/SL grid)
./build/signal_scanner DOGE 2000 both 2      # coin, days, direction, max_combo
./build/signal_scanner BTC 2000 long 3        # BTC long combos up to 3 signals
./build/signal_scanner ETH 2000 both 2        # ETH scan

# Regime analyzer (per-regime signal analysis, walk-forward, Monte Carlo)
./build/regime_analyzer ETH 2000 1h                    # basic analysis
./build/regime_analyzer ETH 2000 1h --validate --montecarlo  # full pipeline
```

## Key Files by Domain

**Engine lifecycle**: `src/core/engine.c` (create/start/stop/destroy, wires all subsystems)
**Config**: `src/core/config.c` + `config/bot_config.json` + `.env` (optional API keys)
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
**Signal scanner**: `tools/signal_scanner.c` (C-native: 65 signals × combos × TP/SL grid, walk-forward, sorted by OOS Sharpe)
**Regime analyzer**: `tools/regime_analyzer.c` (C-native: per-regime signal analysis, walk-forward, Monte Carlo)
**Funding fetcher**: `tools/funding_fetcher.c` (Binance funding rates → SQLite, incremental)
**Grid search**: `scripts/grid_search.sh` (TP/SL grid search wrapper for backtest_json)

## Configuration

- **Env**: `.env` file (TB_ANTHROPIC_API_KEY for AI digest — optional)
- **Config**: `config/bot_config.json` — exchange URLs, risk params, active strategies+coins, paper mode
- **Multi-coin**: `"coins": ["ETH","SOL"]` in strategy config, engine injects `COIN` global
- **Coin exclusivity**: each coin must belong to exactly one strategy — enforced at engine startup (rejects duplicates), GUI Settings (grays out taken coins), and Lua (no cross-strategy guards needed)
- **Paper mode**: always forced to `true` at 4 levels (config.c, engine.c, order_manager.c, GUI IPC). Live trading is disabled. Lua global `PAPER_MODE` is always `true`.
- **Risk (%-based)**: daily_loss_pct=6, emergency_close_pct=5, max_leverage=10, max_position_pct=700

## Conventions

### C to Lua Indicator Fields
`rsi`, `bb_upper`, `bb_middle`, `bb_lower`, `bb_width`, `bb_squeeze`, `sma_20`, `sma_50`, `sma_200`, `ema_12`, `ema_26`, `macd`, `macd_signal`, `macd_histogram`, `atr`, `vwap`, `adx_14`, `plus_di`, `minus_di`, `kc_upper/middle/lower`, `dc_upper/middle/lower`, `stoch_rsi_k/d`, `cci_20`, `williams_r`, `obv`, `obv_sma`, `ichi_tenkan/kijun/senkou_a/senkou_b/chikou`, `cmf`, `mfi`, `squeeze_mom`, `squeeze_on`, `roc`, `zscore`, `fvg_bull`, `fvg_bear`, `fvg_size`, `supertrend`, `supertrend_up`, `psar`, `psar_up`
Aliases (backtest-compatible): `sma`=sma_20, `ema`/`ema_fast`=ema_12, `ema_slow`=ema_26, `bb_mid`=bb_middle

### Lua Funding Rate / Open Interest API
- `bot.get_funding_rate(coin)` → `{rate, premium, mark_px}` or `nil`
  - **Live**: 60s cache, real-time from Hyperliquid
  - **Backtest**: historical data from SQLite `funding_rates` table (Binance, every 8h). `premium` always 0 in backtest.
  - Requires: `./build/funding_fetcher --coins BTC,ETH,SOL,DOGE` to populate DB
- `bot.get_open_interest(coin)` → `number` or `nil` (live-only, returns nil in backtest)

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
- **Funding rates**: loaded from `funding_rates` table in same DB, binary-searched per 5m tick (O(log n))

### Lua Strategy Features
- **GRID_TP/GRID_SL**: Lua globals injected by backtest engine for grid search. Strategies use `GRID_TP or default_tp` syntax.
- **Trailing stop**: Configurable via `trail_activate`, `trail_offset`, `trail_step` in config. Logic in `on_tick` when `in_position`. Ratchets SL by `trail_step` increments. Reset on exit (`on_fill`, `on_timer`).
- **ATR-adaptive sizing**: `atr_baseline`, `atr_size_min`, `atr_size_max` in config. Multiplier = baseline/current_atr, clamped [min, max]. Applied in `get_trade_size()`.

### Critical Gotchas
- MACD needs 34+ candles: use `get_indicators(coin, tf, 50, mid)` minimum
- `ind.valid` does NOT exist: test `if not ind then return end`
- `bb_middle` not `bb_mid` from C (backtest has alias)
- `get_indicators` 4th arg = live_price (injects into last candle close)
- **1 coin = 1 strategy**: never assign the same coin to multiple strategies (engine refuses to start)

## Dependencies (macOS)
libcurl, OpenSSL, libwebsockets, Lua 5.4, libsecp256k1, msgpack-c, SQLite3 (all via Homebrew). yyjson via CMake FetchContent.

## Strategies (paper — educational)
- **BTC**: `btc_sniper_1h.lua` — ultra-selective high-conviction (x7, 90% equity, 2 signals L1+S1, ATR<0.4%, TP/SL 2.0%/2.0%)
- **DOGE**: `doge_sniper_relaxed_1h.lua` — low-vol sniper RR 3:1 (x5, 40% equity, 4 signals L1+L3+S1+S2, ATR<0.9%, TP/SL 4.5%/1.5%, trailing stop, ATR-adaptive sizing)
- **SOL**: `sol_range_breakout_1h.lua` v2.0.0 — range breakout + bear complement (x5, 40% equity, RB TP/SL 4.5%/1.5%, B1 TP/SL 6.0%/2.0%, trailing stop, ATR-adaptive sizing)
- **ETH**: aucune strategie active — en recherche via signal_scanner
- **Template**: `strategy_template.lua` — skeleton for new strategies
- Coins: BTC, DOGE, SOL (1 coin = 1 strategy instance, exclusive)
- Risk: daily_loss 6%, emergency 5%, max_leverage 10x, max_position 700%
- **Features**: trailing stop (configurable activate/offset/step), ATR-adaptive sizing, grid search TP/SL
- **Mode: PAPER (educational build — live trading disabled)**

## Analysis Pipeline

### C Signal Scanner (primary — recommended)
- `tools/signal_scanner.c` → `./build/signal_scanner` — 65 signals, combos 1-3, TP/SL grid, walk-forward IS/OOS
- Directement sur les indicateurs C (`tb_indicators_compute`), 0 gap avec le backtest engine
- Output: TSV trié par OOS Sharpe, top 500 résultats
- Usage: `./build/signal_scanner COIN [n_days] [long|short|both] [max_combo]`

### Grid Search TP/SL
- `scripts/grid_search.sh` — boucle TP (1.0→6.0) × SL (0.5→4.0), appelle backtest_json avec override GRID_TP/GRID_SL
- Injecté via `backtest_engine.c` → globals Lua GRID_TP/GRID_SL, strategies lisent avec fallback

### C Regime Analyzer
- `tools/regime_analyzer.c` → `./build/regime_analyzer` — per-regime signal analysis
- Classifie bull/bear/neutral (SMA200 + EMA12/26 + ADX + DI, hysteresis 3 bars)
- 65 signaux × combos 1-2 × grille TP/SL, filtré par régime
- Walk-forward 5-split (60% overlap, 70/30 train/test) avec `--validate`
- Monte Carlo bootstrap 10k sims (xoshiro256**) avec `--montecarlo`
- Output: rapport markdown → `data/analysis/{COIN}_regime_report.md`
- Usage: `./build/regime_analyzer COIN [n_days] [tf] [--validate] [--montecarlo]`

### C Funding Fetcher
- `tools/funding_fetcher.c` → `./build/funding_fetcher`
- Binance `fapi/v1/fundingRate` (public, pas d'auth), pagination batch 1000
- Incrémental (reprend depuis MAX(time_ms)), rate limiting 200ms
- Usage: `./build/funding_fetcher --coins BTC,ETH,SOL,DOGE --days 3200`

### Backtest Monte Carlo
- Intégré dans `backtest_json.c` — 10k simulations bootstrap sur les trades du backtest
- Output JSON `monte_carlo`: p_ruin, p95/p99 drawdown, median/p5/p95 return, equity finale
- Pas de flag requis — automatique dès qu'il y a >= 5 trades

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
