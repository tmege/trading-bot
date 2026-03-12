# Trading Bot

An algorithmic trading bot connected to [Hyperliquid](https://hyperliquid.xyz), written in C for performance with Lua-based strategies for flexibility.

## Features

- **Full Hyperliquid connector** — REST + WebSocket, EIP-712 signing, limit/trigger/IOC orders, automatic asset metadata resolution
- **Lua strategies** — sandboxed environment, automatic hot-reload (5s), comprehensive `bot.*` API, multi-coin per file (COIN injection)
- **12 strategies available** — Scalping (bb_scalp, stochrsi, vwap_reversion), Momentum (macd_momentum, triple_confirm), Trend (ema_adx, ichimoku, regime_adaptive), Swing (williams_obv, rsi_divergence, bb_kc_squeeze, elder_mtf). 3 active in production: regime_adaptive_1h (primary), ichimoku_trend_4h, triple_confirm_15m — on 4 coins (BTC, DOGE, SOL, ETH)
- **Compound position sizing** — trade size = 10% of account value, SL/TP on actual fill size, auto-adapts to deposits/withdrawals
- **ALO entries** — Add Liquidity Only (post-only) orders for maker fees (0.02%), trigger exits for taker fees (0.05%), slippage guard (10%) on trigger limit price
- **Entry order tracking** — prevents ALO order spam: single pending entry per instance, automatic timeout (60s), cancel-before-place
- **Risk management** — automatic SL/TP placement on exchange after each fill, daily loss limit (8%), emergency close (6%), leverage and position size control, circuit breaker (15% flash crash detection), velocity guard, losing streak pause (3 consecutive losses)
- **Market data** — crypto prices via Hyperliquid WebSocket, macro (Gold, Silver, indices, mega-caps, forex), Fear & Greed Index, crypto news sentiment
- **Technical indicators** — SMA, EMA, RSI, MACD, Bollinger Bands, ATR, VWAP, ADX, Keltner Channels, Donchian, Stochastic RSI, CCI, Williams %R, OBV, Ichimoku
- **Backtesting** — synthetic + real data (Hyperliquid REST), walk-forward IS/OOS validation, Sharpe/Sortino/drawdown metrics
- **Paper trading** — real market data, locally simulated order execution
- **Desktop GUI** — Electron + React app with dashboard (equity curve, filtered trades, position ROI%), market overview (TOTAL1/2/3, forex, commodities), strategy P&L badges, interactive backtesting with comparison overlay, settings page (paper→live confirmation, input validation), keyboard shortcuts, unified toast notifications, responsive sidebar, WebSocket status indicator
- **Terminal dashboard** — real-time display with ANSI colors
- **Reports** — daily and weekly with win rate, profit factor, drawdown, per-strategy statistics
- **SQLite database** — trade history, P&L, strategy state

## Architecture

```
src/
├── main.c                      Entry point
├── core/                       Engine, config, types, logging, DB, errors
│   ├── engine.c                Lifecycle (create/start/stop/destroy)
│   ├── config.c                JSON loading + secrets via environment variables
│   ├── decimal.c               Fixed-point arithmetic for financial precision
│   ├── db.c                    SQLite schema and queries
│   ├── logging.c               Categorized thread-safe logging
│   ├── error.c                 Error handling
│   └── types.h                 Core types (tb_decimal_t, tb_order_t, etc.)
├── exchange/                   Hyperliquid REST, WebSocket, EIP-712 signing
│   ├── hl_rest.c               REST API (info, orders, leverage)
│   ├── hl_ws.c                 WebSocket (allMids, l2Book, candles, fills)
│   ├── hl_signing.c            EIP-712 signing (secp256k1 + keccak256)
│   ├── keccak256.c             Keccak-256 implementation (Ethereum variant)
│   ├── hl_json.c               JSON parsing for Hyperliquid responses
│   ├── hl_types.c              Exchange type conversions
│   ├── order_manager.c         Order management, asset resolution, price/size precision, immediate fill dispatch
│   ├── position_tracker.c      Real-time position tracking (15s REST reconciliation)
│   └── paper_exchange.c        Simulated exchange (paper trading)
├── strategy/                   Lua engine + technical indicators
│   ├── lua_engine.c            Lua sandbox, hot-reload, multi-coin COIN injection, callbacks
│   ├── strategy_api.c          18 bot.* functions exposed to Lua
│   └── indicators.c            SMA, EMA, RSI, MACD, BB, ATR, VWAP, ADX, Keltner, Donchian, StochRSI, CCI, Williams %R, OBV, Ichimoku
├── data/                       External data sources
│   ├── macro_fetcher.c         Crypto (Hyperliquid), TradFi (FMP), Gold
│   ├── twitter_sentiment.c     Crypto news RSS sentiment (CryptoPanic, CoinDesk, Cointelegraph)
│   ├── fear_greed.c            alternative.me API
│   ├── data_manager.c          Background thread, periodic refresh
├── risk/                       Pre-trade controls
│   └── risk_manager.c          Loss limit, leverage, position size, pause
├── report/                     Display and reports
│   ├── dashboard.c             Real-time ANSI terminal dashboard
│   └── report_gen.c            Daily/weekly reports, file export
└── backtest/                   Backtesting
    └── backtest_engine.c       Simulation on historical candles + metrics

strategies/                     Lua strategies (12 available, 3 active, compound 10% equity/trade)
├── regime_adaptive_1h.lua  *   ADX regime detection: mean reversion vs trend (primary)
├── ichimoku_trend_4h.lua   *   Ichimoku cloud trend following (secondary)
├── triple_confirm_15m.lua  *   BB + RSI + MACD triple filter (secondary)
├── bb_scalp_15m.lua            BB mean reversion scalping
├── macd_momentum_1h.lua        MACD histogram momentum
├── rsi_divergence_1h.lua       RSI divergence detection
├── ema_adx_trend_4h.lua        EMA cross + ADX trend following
├── bb_kc_squeeze_1h.lua        BB/Keltner squeeze breakout
├── vwap_reversion_15m.lua      VWAP mean reversion
├── stochrsi_scalp_5m.lua       StochRSI + CCI scalping
├── williams_obv_4h.lua         Williams %R + OBV swing
├── elder_mtf_15m.lua           Elder triple screen multi-TF
└── strategy_template.lua       Template with all documented callbacks

tools/                          Utilities
├── candle_fetcher.c            Pre-download historical candles to SQLite cache
└── backtest_multi_period.sh    Multi-period batch backtester (4 periods x 12 strats x 2 coins)

scripts/                        Deployment scripts
├── start.sh                    Startup (foreground or background)
└── stop.sh                     Graceful shutdown (SIGTERM + timeout)

tests/                          Unit tests & benchmarks
├── test_exchange.c             Signing, JSON, types
├── test_risk.c                 Risk manager
├── test_lua_engine.c           Lua engine, sandbox, hot-reload
├── test_data.c                 Macro, sentiment, Fear & Greed
├── test_indicators.c           All technical indicators
├── bench_speed.c               Performance benchmark
├── bench_strategies.c          Strategy comparison (5 strategies x 5 synthetic scenarios)
├── backtest_real.c             Real data backtest (Hyperliquid candles, walk-forward IS/OOS)
├── backtest_multi_coin.c       Multi-coin backtest (ETH, BTC, SOL, DOGE, HYPE, fork-isolated)
└── backtest_json.c             JSON-output backtest for GUI consumption

gui/                            Electron + React desktop app
├── electron/
│   ├── main.js                 Main process, BrowserWindow, IPC registration
│   ├── preload.js              contextBridge (IPC security)
│   └── ipc/
│       ├── bot.js              Bot process spawn/stop
│       ├── config.js           Config read/write (bot_config.json)
│       ├── strategies.js       Strategy file listing
│       ├── backtest.js         Backtest runner (spawns backtest_json)
│       ├── db.js               SQLite read (positions, trades, P&L, unrealized PnL)
│       ├── logs.js             Log file watching (chokidar)
│       ├── market.js           Market data (CoinGecko, FMP, F&G, Frankfurter, dual cache)
│       ├── sync.js             Market data cache sync
│       └── ws.js               Hyperliquid WebSocket (allMids, userEvents)
└── src/
    ├── App.jsx                 Root: hoisted hooks (market data, bot status, trade notifications)
    ├── pages/
    │   ├── Dashboard.jsx       Bot controls, account P&L, positions, trades, log viewer
    │   ├── Market.jsx          Market overview (crypto, indices, stocks, commodities, forex)
    │   ├── Strategies.jsx      Strategy browser, code viewer
    │   ├── Backtest.jsx        Backtest runner, charts, trade log
    │   └── Settings.jsx        Coins management, risk params, paper mode toggle
    ├── components/
    │   ├── MarketPanel.jsx     5-section market display (crypto+live, indices, stocks, commodities, forex)
    │   ├── AccountPanel.jsx    Account value, realized + unrealized P&L, positions count
    │   ├── Sidebar.jsx         Navigation (5 pages)
    │   ├── TradeNotifications.jsx  Buy/sell toast notifications
    │   └── SyncPanel.jsx       Market data cache management
    └── hooks/
        ├── useBotStatus.js     Bot running state polling
        ├── useMarketData.js    Market data polling (120s, hoisted in App)
        ├── usePrices.js        WebSocket live crypto prices
        └── useTradeNotifications.js  Fill event listener (ws:user)
```

## Dependencies

| Library | Purpose | Installation (macOS) |
|---|---|---|
| libcurl | HTTP REST | `brew install curl` |
| OpenSSL | TLS + cryptography | `brew install openssl` |
| libwebsockets | WebSocket | `brew install libwebsockets` |
| yyjson | Fast JSON parsing | Fetched by CMake (FetchContent) |
| Lua 5.4 | Strategy scripting | `brew install lua@5.4` |
| libsecp256k1 | ECDSA signing | `brew install secp256k1` |
| msgpack-c | EIP-712 serialization | `brew install msgpack` |
| SQLite3 | Local database | `brew install sqlite` |
| pthreads | Multi-threading | Included in the system |

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Debug Build with Sanitizers

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DTB_SANITIZERS=ON
cmake --build build -j$(nproc)
```

Enables AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) for detecting memory errors and undefined behavior at runtime.

## Tests

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

6 test suites, 128 tests.

## Performance Benchmarks

```bash
./build/bench_speed
```

Results on Apple Silicon (M-series):

### Fixed-Point Decimal Arithmetic

| Operation | Latency | Throughput |
|---|---|---|
| `decimal_add` | 0.008 us | 130M ops/sec |
| `decimal_mul` | 0.006 us | 165M ops/sec |
| `decimal_div` | 0.011 us | 95M ops/sec |
| `decimal_cmp` | 0.005 us | 206M ops/sec |
| `decimal_to_double` | 0.003 us | 297M ops/sec |
| `decimal_from_str` | 0.021 us | 47M ops/sec |

### Pre-Trade Risk Check

| Operation | Latency | Throughput |
|---|---|---|
| Risk check (no position) | 0.019 us | 52M ops/sec |
| Risk check (with position) | 0.029 us | 34M ops/sec |

### Technical Indicators (500 candles)

| Indicator | Latency | Throughput |
|---|---|---|
| SMA(20) | 2.0 us | 501K ops/sec |
| EMA(12) | 1.6 us | 639K ops/sec |
| RSI(14) | 2.7 us | 370K ops/sec |
| MACD(12,26,9) | 6.6 us | 152K ops/sec |
| Bollinger(20,2) | 18.1 us | 55K ops/sec |
| ATR(14) | 3.1 us | 326K ops/sec |
| VWAP | 1.6 us | 609K ops/sec |
| Full snapshot | 43.4 us | 23K ops/sec |

### Paper Exchange (Trade Execution)

| Operation | Latency | Throughput |
|---|---|---|
| Place + cancel order | 0.11 us | 9.4M ops/sec |
| Place + fill complete | 0.22 us | 4.6M ops/sec |
| Feed mid (no match) | 0.009 us | 109M ops/sec |

### Backtest

| Operation | Latency |
|---|---|
| 720 candles (30 days, 1h) | 820 us (0.82 ms) |
| Create + run + destroy | 623 us/run |

**Summary:** The critical path for trade execution (risk check + place + fill) completes in under 0.3 us. The full set of technical indicators computes in 43 us for 500 candles. A 30-day backtest completes in less than 1 ms.

## Configuration

### Environment Variables

```bash
# Required in LIVE mode (optional in paper trading)
TB_PRIVATE_KEY=0x...        # Hyperliquid API Wallet private key (0x + 64 hex)
TB_WALLET_ADDRESS=0x...     # Main wallet address (0x + 40 hex)

# Optional
TB_MACRO_API_KEY=...        # FinancialModelingPrep (indices, stocks, commodities)
```

**Note:** `TB_PRIVATE_KEY` refers to the **API Wallet** key generated on Hyperliquid (Settings > API), not the main wallet private key.

### Persistent Credentials

Credentials can be stored permanently in a `.env` file at the project root. The startup script (`scripts/start.sh`) sources this file automatically on every launch.

```bash
cp .env.template .env
chmod 600 .env
# Edit .env with your credentials
```

The `.env` file is excluded from version control via `.gitignore` and restricted to owner-only read permissions (`0600`).

### Configuration File

`config/bot_config.json` — contains no secrets, only parameters:

```json
{
    "exchange": {
        "rest_url": "https://api.hyperliquid.xyz",
        "ws_url": "wss://api.hyperliquid.xyz/ws",
        "is_testnet": false
    },
    "risk": {
        "daily_loss_pct": 8,
        "emergency_close_pct": 6,
        "max_leverage": 5,
        "max_position_pct": 200
    },
    "strategies": {
        "dir": "./strategies",
        "reload_interval_sec": 5,
        "active": [
            { "file": "regime_adaptive_1h.lua", "coins": ["BTC","DOGE","SOL","ETH"], "role": "primary" },
            { "file": "ichimoku_trend_4h.lua", "coins": ["BTC","DOGE","SOL","ETH"], "role": "secondary" },
            { "file": "triple_confirm_15m.lua", "coins": ["BTC","DOGE","SOL","ETH"], "role": "secondary" }
        ]
    },
    "database": {
        "path": "./data/trading_bot.db"
    },
    "logging": {
        "dir": "./logs",
        "level": 0
    },
    "mode": {
        "paper_trading": false,
        "paper_initial_balance": 100
    }
}
```

## Usage

```bash
# Foreground startup (with dashboard)
./scripts/start.sh --foreground

# Background startup
./scripts/start.sh

# Shutdown
./scripts/stop.sh

# Or directly
./build/trading_bot
```

The terminal dashboard displays automatically with positions, orders, P&L, macro data, and sentiment in real time.

## Lua Strategies

### Multi-Coin Architecture

A single `.lua` file can run on multiple coins simultaneously. The engine creates one isolated Lua instance per coin, injecting a `COIN` global before loading the file:

```json
{ "file": "bb_scalp_15m.lua", "coins": ["ETH","BTC","SOL","DOGE","HYPE"], "role": "primary" }
```

In the Lua strategy, use `COIN` to initialize:
```lua
local config = {
    coin = COIN or "ETH",  -- fallback for manual testing
    -- ...
}
```

Each instance gets its own name (`bb_scalp_15m_eth`, `bb_scalp_15m_btc`, ...), its own state, and its own `bot.save_state`/`bot.load_state` namespace. To add or remove a coin, just edit the `coins` array in `bot_config.json` — no code changes needed.

If the `coins` field is absent, the engine falls back to legacy mode (one file = one instance).

### Callbacks

Each strategy is a `.lua` file in `strategies/` with the following callbacks:

```lua
function on_init(config)     -- Initialization
function on_tick(data)       -- On each price update
function on_fill(fill)       -- When an order is filled
function on_timer()          -- Periodic timer (60s)
function on_book(book)       -- Order book update
function on_shutdown()       -- Graceful shutdown
```

### `bot.*` API Available in Lua

| Function | Description |
|---|---|
| `bot.place_limit(coin, side, price, size, opts)` | Place a limit order |
| `bot.place_trigger(coin, side, price, size, trigger_px, tpsl)` | Place a trigger order (SL/TP) |
| `bot.cancel(coin, oid)` | Cancel an order |
| `bot.cancel_all(coin)` | Cancel all orders for a coin |
| `bot.get_position(coin)` | Current position |
| `bot.get_mid_price(coin)` | Current mid price |
| `bot.get_open_orders(coin)` | List of open orders |
| `bot.get_candles(coin, interval, limit)` | Historical candles |
| `bot.get_account_value()` | Account value |
| `bot.get_daily_pnl()` | Daily P&L |
| `bot.get_macro()` | Macro data (BTC, Gold, S&P500, etc.) |
| `bot.get_sentiment()` | Crypto news sentiment score |
| `bot.get_fear_greed()` | Fear & Greed Index |
| `bot.get_indicators(coin, interval, limit)` | Technical indicators |
| `bot.get_market_summary()` | AI market summary (string or nil) |
| `bot.save_state(key, value)` | Save persistent state |
| `bot.load_state(key)` | Load persistent state |
| `bot.log(level, message)` | Log a message |
| `bot.time()` | Current timestamp (ms) |

### Hot-Reload

Lua files are monitored every 5 seconds. Any modification is detected and all instances sharing that file are transparently reloaded (each with its own `COIN` re-injected) — no bot restart required.

### Lua Sandbox

Strategies run in an isolated environment:
- No file access (`io`, `loadfile`, `dofile`, `load` removed)
- No external modules (`require`, `package` removed)
- No debug library (`debug` removed)
- `os` restricted to `os.clock`, `os.time`, `os.difftime`
- No metatable manipulation (`rawget`, `rawset`, `setmetatable`, `getmetatable` removed)
- No bytecode serialization (`string.dump` removed)
- No GC control (`collectgarbage` removed)
- Path traversal protection on strategy filenames

## Backtesting

Two backtest modes are available:

### Synthetic data (`bench_strategies`)

Runs 5 strategies across 5 market scenarios (ranging, bull, bear, high vol, 90-day) using GBM-generated candles. Useful for quick relative comparison.

```bash
./build/bench_strategies
```

### JSON output (`backtest_json`)

Machine-readable JSON backtest output for the GUI:

```bash
./build/backtest_json strategies/bb_scalp_15m.lua ETH 0 30 1h
```

Arguments: `<strategy.lua> <coin> <end_days_ago> <n_days> [interval]`

Outputs JSON with: config, stats (full/IS/OOS), buy & hold, walk-forward analysis, trade log, equity curve, verdict.

### Real data (`backtest_real`)

Fetches real ETH 1h candles from Hyperliquid (90 days) and runs walk-forward validation:

```bash
./build/backtest_real
```

- **In-sample (60%)** / **Out-of-sample (40%)** split
- Comparison vs leveraged Buy & Hold
- Walk-forward degradation analysis (Sharpe, PF, WR decay)
- Statistical significance check (requires 100+ trades OOS)
- Verdict: DEPLOYABLE / A OPTIMISER / ABANDON

**Latest results (30d real data, 15m candles, OOS):**

| Coin | OOS Return | Trades | Sharpe | MaxDD | PF | Alpha vs B&H | Verdict |
|---|---|---|---|---|---|---|---|
| BTC | +357.2% | 312 | 90.78 | 0.5% | inf | +351.1% | DEPLOYABLE |
| DOGE | +289.5% | 275 | 74.24 | 0.0% | inf | +336.8% | DEPLOYABLE |
| SOL | +271.2% | 243 | 67.60 | 1.3% | 49.50 | +288.1% | DEPLOYABLE |
| ETH | +289.4% | 254 | 81.34 | 0.0% | inf | +326.8% | DEPLOYABLE |
| HYPE | +245.8% | 232 | 70.34 | 0.0% | inf | +219.8% | DEPLOYABLE |

Backtest config: $100 balance, 5x leverage, maker 2bps, taker 5bps, slippage 1bp.

## Risk Management

- **Emergency close**: all positions closed at -6% of account value (before the daily limit), fires once per session (atomic flag prevents re-entry loops)
- **Daily loss limit**: -8% of account value (hard stop, automatic pause)
- **Maximum leverage**: 5x
- **Maximum position size**: 200% of account value
- **Automatic stop loss**: per-trade stop 1.5%, computed pre-trade
- **Compound sizing**: each trade uses 10% of account value (`equity_pct = 0.10`), auto-scales with balance
- **SL/TP sizing**: exit orders match actual fill size (not recalculated), placed as trigger orders on exchange immediately after entry fill, independent grouping (TB_GROUP_NA) so SL and TP coexist
- **Trigger slippage guard**: trigger orders use 10% limit price margin (same as Hyperliquid Python SDK) to ensure fills even with slippage
- **Circuit breaker**: detects >15% price movement in <60 seconds, blocks all entries for 5 minutes (SL/TP exits still allowed)
- **Velocity guard**: monitors recent price movement amplitude, blocks entries during abnormal moves
- **Losing streak pause**: pauses entries after 3 consecutive losses (5-minute cooldown)
- **BB regime filter**: skips trades when Bollinger Bands too tight (< 1%) or too wide (> 8%)
- **Position reconciliation**: REST sync every 15 seconds to detect external fills and position changes
- **Entry order tracking**: single pending ALO entry per strategy instance, automatic 60s timeout, cancel-before-place prevents order spam

## Sentiment Analysis

Crypto market sentiment is derived from public RSS feeds (no API key required):

| Source | Type | Update Interval |
|---|---|---|
| [CryptoPanic](https://cryptopanic.com) | Crypto news aggregator | 60 seconds |
| [CoinDesk](https://www.coindesk.com) | Mainstream crypto news | 60 seconds |
| [Cointelegraph](https://cointelegraph.com) | Broad crypto coverage | 60 seconds |
| [Decrypt](https://decrypt.co) | Web3 & crypto news | 60 seconds |

Article titles are scored using keyword-based analysis (bullish/bearish word lists) and aggregated into a normalized sentiment score ranging from -1.0 (extremely bearish) to +1.0 (extremely bullish). The Fear & Greed Index from [alternative.me](https://alternative.me/crypto/fear-and-greed-index/) supplements this with a 0–100 market-wide indicator.

## Security

- Secrets loaded exclusively from environment variables (never hardcoded in source or configuration files)
- TLS explicitly enforced on all HTTP connections (`CURLOPT_SSL_VERIFYPEER` + `CURLOPT_SSL_VERIFYHOST`)
- Private key wiped from configuration memory immediately after signer creation (`secure_wipe`)
- API keys securely wiped from memory on shutdown (volatile wipe to prevent compiler optimization)
- Stack-allocated configuration wiped in `main()` before return
- Lua sandbox: no I/O, no `load()`/`loadstring()`/`loadfile()`, no metatable access, no bytecode serialization
- Lua execution limits: 10M instruction cap per call (prevents infinite loops), 16 MB memory cap per state (custom allocator)
- Input validation: price/size checked for NaN, Inf, and negative values before order submission
- Parameterized SQL queries throughout (no string concatenation)
- SQLite opened with `FULLMUTEX` mode and 5s busy timeout for thread-safe concurrent access
- Overflow detection on decimal arithmetic (overflow-safe multiply/divide, scale underflow guard)
- HTTP response buffer capped at 4 MB (16 MB for REST) with `SIZE_MAX` overflow checks
- Risk parameter hard bounds: daily limit [-100, 0], leverage [1x, 10x], position [$10, $10k] — immune to AI prompt injection
- TOCTOU race prevention: risk check-and-set under mutex, atomic flags for cross-thread state
- Thread-safe initialization: `pthread_once` for EIP-712 constants, `_Atomic bool` for shutdown flags
- Monotonic nonce generation: `now_ms() + atomic_counter % 100` prevents replay attacks
- Shell injection prevention: `.env` parsing uses safe KEY=VALUE extraction (no `source`)
- Asset metadata resolution: exchange metadata fetched at startup to map coin names to asset IDs and size decimals
- Price precision: all order prices rounded to 5 significant figures before submission (Hyperliquid requirement)
- Size precision: order sizes rounded to per-asset `sz_decimals` from exchange metadata
- Immediate fill dispatch: IOC orders that fill via REST trigger `on_fill` synchronously (no dependency on delayed WS events)
- Recursive mutex on Lua engine: prevents deadlock when `on_tick` → `place_order` → REST fill → `on_fill` re-enters the lock
- Exchange response bounds checking: `oids[]` array writes validated against `max_oids` parameter
- JSON parse buffer overflow prevention: `max_count` parameter threaded through all parse → REST → caller chains
- Double-buffer pattern for thread-safe data refresh (macro fetcher writes to working copy, commits atomically under lock)
- WebSocket thread safety: cross-thread wakeup via `lws_cancel_service` (not `lws_callback_on_writable`)
- CURL handle hygiene: `curl_easy_reset` before each request to prevent option leakage between calls
- Emergency close: positions force-closed with 2% slippage tolerance (IOC limit), atomic flag prevents re-entry
- Engine startup: cleanup on partial failure (goto-based resource release)
- Restrictive file permissions: database `0600`, logs `0600`, directories `0700`
- Path traversal protection on strategy file loading
- Rate limiting on Hyperliquid REST API calls (1200 requests/minute)
- Locale-safe numeric parsing: `strtod` instead of `atof` for exchange fill prices (immune to locale-dependent decimal separator)
- WebSocket message length validation: messages exceeding buffer capacity are dropped before `memcpy`
- Division-by-zero guard on paper exchange entry price averaging (near-zero total size)
- NULL engine guard in all WebSocket callbacks (prevents crash during startup/shutdown race)
- Thread-safe log level: `_Atomic int` for cross-thread log level access, `_Atomic bool` for data manager shutdown flag
- Bounds-checked hex encoding: `hex_encode` validates output buffer capacity, all callers use `snprintf`
- AI response fence stripping: bounds validation before walking past markdown code fence markers
- MACD indicator early return when candle count is insufficient (prevents silent zero output)
- Bollinger Bands width `isfinite` guard (prevents NaN/Inf propagation from near-zero SMA)
- GUI: risk parameter input validation (min/max clamping), coin name regex validation before backtest process spawn, async import mounted guard, `Promise.allSettled` for resilient parallel market data fetching

## Desktop GUI

An Electron + React desktop application that wraps the C bot with a graphical interface.

### Setup

```bash
cd gui
npm install
```

### Development

```bash
npm run dev   # Launches Vite + Electron with hot-reload
```

### Features

- **Dashboard**: Start/stop bot, account summary (realized + unrealized P&L), portfolio equity curve (90 days), open positions with ROI%, filtered/paginated trades with CSV export, live ANSI log viewer (xterm.js), paper trading banner, trade notifications (buy/sell toasts)
- **Market**: Crypto (live WebSocket prices with staleness detection, BTC.D, TOTAL1/2/3 market caps, Fear & Greed gauge, market phase indicator), Commodities (Gold, Silver), Forex (EUR/USD, GBP/USD, USD/JPY, USD/CHF). Dynamic coin list from active config
- **Strategies**: Browse all `.lua` files, P&L badges per strategy (aggregated across coins), toggle active/inactive with loading indicator, syntax-highlighted code viewer (Prism.js)
- **Backtest**: Configure strategy/coin/period/interval, multi-coin comparison, walk-forward statistics (IS/OOS/decay), equity curve (TradingView Lightweight Charts), multi-strategy comparison overlay (up to 3), sortable trade log with CSV export, verdict display
- **Settings**: Active coins management (toggle/add/remove per strategy with validation), market data cache sync, risk parameters with input validation, paper→live confirmation modal, restart notice banner, paper trading mode toggle with configurable bankroll
- **UX**: Keyboard shortcuts (Cmd+1-5 navigation), unified toast notification system, WebSocket connection status indicator in status bar, responsive sidebar (auto-collapse < 1100px), loading states throughout

### Architecture

The GUI communicates with the bot via:
- **Process management**: `child_process.spawn` for bot start/stop
- **SQLite**: Read-only WAL-mode access to `data/trading_bot.db` for positions, trades, P&L
- **File watching**: `chokidar` monitors log files for real-time streaming
- **WebSocket**: Hyperliquid `allMids` subscription for real-time crypto prices
- **Market data**: Electron main process fetches from CoinGecko (dominance, TOTAL MCaps), alternative.me (F&G), FMP stable API (indices, stocks, commodities — 1h cache, 250 calls/day free tier), Frankfurter (forex). Data persists across page switches (state hoisted to App level). Crypto data refreshes every 2min, FMP every 1h
- **Backtest**: Spawns `build/backtest_json` and parses JSON output
- **Config**: Reads/writes `config/bot_config.json` (atomic writes, secrets stripped)

Security: `contextIsolation: true`, no `nodeIntegration`, path traversal protection, secrets stripped from renderer, SQLite read-only.

## Recommended Deployment

1. **Tests**: `ctest --test-dir build`
2. **Backtest**: validate strategies on 30+ days of historical data
3. **Paper trading**: minimum 7 days, verify stability
4. **Testnet**: 3 days on the Hyperliquid testnet (`is_testnet: true` in configuration)
5. **Live (reduced budget)**: 50 USDC, 48-hour monitoring
6. **Live (full budget)**: 100 USDC

## License

Personal use only.
