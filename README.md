# Trading Bot

An algorithmic trading bot connected to [Hyperliquid](https://hyperliquid.xyz), written in C for performance with Lua-based strategies for flexibility.

## Features

- **Full Hyperliquid connector** — REST + WebSocket, EIP-712 signing, limit/trigger/IOC orders, automatic asset metadata resolution
- **Lua strategies** — sandboxed environment, automatic hot-reload (5s), comprehensive `bot.*` API
- **Active strategies** — BB Scalping on 5 coins (ETH, BTC, SOL, DOGE, HYPE) — Bollinger Bands mean reversion, validated on 30 days real data (15m candles)
- **Risk management** — automatic SL/TP placement on exchange after each fill, daily loss limit, leverage and position size control
- **Market data** — crypto prices via Hyperliquid, macro (Gold, S&P500, DXY), Fear & Greed Index, crypto news sentiment
- **Technical indicators** — SMA, EMA, RSI, MACD, Bollinger Bands, ATR, VWAP + derived signals
- **AI advisory** — Claude Haiku called on startup + twice daily (8h/20h UTC) to analyze positions and suggest adjustments
- **Backtesting** — synthetic + real data (Hyperliquid REST), walk-forward IS/OOS validation, Sharpe/Sortino/drawdown metrics
- **Paper trading** — real market data, locally simulated order execution
- **Terminal dashboard** — real-time display with ANSI colors
- **Reports** — daily and weekly with win rate, profit factor, drawdown, per-strategy statistics
- **SQLite database** — trade history, P&L, advisory logs

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
│   ├── lua_engine.c            Lua sandbox, hot-reload, callbacks
│   ├── strategy_api.c          18 bot.* functions exposed to Lua
│   └── indicators.c            SMA, EMA, RSI, MACD, BB, ATR, VWAP
├── data/                       External data sources
│   ├── macro_fetcher.c         Crypto (Hyperliquid), TradFi (FMP), Gold
│   ├── twitter_sentiment.c     Crypto news RSS sentiment (CryptoPanic, CoinDesk, Cointelegraph)
│   ├── fear_greed.c            alternative.me API
│   ├── data_manager.c          Background thread, periodic refresh
│   └── ai_advisor.c            Claude Haiku, JSON context, response parsing
├── risk/                       Pre-trade controls
│   └── risk_manager.c          Loss limit, leverage, position size, pause
├── report/                     Display and reports
│   ├── dashboard.c             Real-time ANSI terminal dashboard
│   └── report_gen.c            Daily/weekly reports, file export
└── backtest/                   Backtesting
    └── backtest_engine.c       Simulation on historical candles + metrics

strategies/                     Lua strategies
├── scalp_eth.lua               BB Scalping ETH [PRIMARY] (BB 20/2, SL=1.5%, TP=3.0%, $40/trade)
├── scalp_btc.lua               BB Scalping BTC (same params, $40/trade)
├── scalp_sol.lua               BB Scalping SOL (same params, $40/trade)
├── scalp_doge.lua              BB Scalping DOGE (same params, $40/trade)
├── scalp_hype.lua              BB Scalping HYPE (same params, $40/trade)
├── momentum_eth.lua            Momentum ETH (EMA12/26, RSI, ATR trailing stop — inactive)
├── grid_eth.lua                Grid Trading ETH (dynamic ATR, 15 levels — inactive)
├── dca_eth.lua                 DCA ETH (RSI-based buy/sell zones — inactive)
├── signal_doge.lua             Signal DOGE (sentiment + volume — inactive)
└── strategy_template.lua       Template with all documented callbacks

scripts/                        Deployment scripts
├── start.sh                    Startup (foreground or background)
└── stop.sh                     Graceful shutdown (SIGTERM + timeout)

tests/                          Unit tests & benchmarks
├── test_exchange.c             Signing, JSON, types
├── test_risk.c                 Risk manager
├── test_lua_engine.c           Lua engine, sandbox, hot-reload
├── test_data.c                 Macro, sentiment, Fear & Greed
├── test_advisor.c              AI advisor, response parsing
├── test_indicators.c           All technical indicators
├── bench_speed.c               Performance benchmark
├── bench_strategies.c          Strategy comparison (5 strategies x 5 synthetic scenarios)
├── backtest_real.c             Real data backtest (Hyperliquid candles, walk-forward IS/OOS)
└── backtest_multi_coin.c       Multi-coin backtest (ETH, BTC, SOL, DOGE, HYPE, fork-isolated)
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
TB_CLAUDE_API_KEY=sk-...    # Claude API key (for AI advisory)
TB_MACRO_API_KEY=...        # FinancialModelingPrep (S&P500, DXY)
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
        "daily_loss_limit_usdc": -15.0,
        "emergency_close_usdc": -12.0,
        "max_leverage": 5,
        "per_trade_stop_pct": 1.5,
        "max_position_usd": 200.0
    },
    "strategies": {
        "dir": "./strategies",
        "reload_interval_sec": 5,
        "active": [
            { "file": "scalp_eth.lua",  "role": "primary" },
            { "file": "scalp_btc.lua",  "role": "secondary" },
            { "file": "scalp_sol.lua",  "role": "secondary" },
            { "file": "scalp_doge.lua", "role": "secondary" },
            { "file": "scalp_hype.lua", "role": "secondary" }
        ]
    },
    "ai_advisory": {
        "model": "claude-haiku-4-5-20251001",
        "morning_hour_utc": 8,
        "evening_hour_utc": 20
    },
    "database": {
        "path": "./data/trading_bot.db"
    },
    "logging": {
        "dir": "./logs",
        "level": 0
    },
    "mode": {
        "paper_trading": false
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

Each strategy is a `.lua` file in `strategies/` with the following callbacks:

```lua
function on_init(config)     -- Initialization
function on_tick(data)       -- On each price update
function on_fill(fill)       -- When an order is filled
function on_timer()          -- Periodic timer (60s)
function on_book(book)       -- Order book update
function on_advisory(adj)    -- AI advisory adjustments
function on_shutdown()       -- Graceful shutdown
```

### `bot.*` API Available in Lua

| Function | Description |
|---|---|
| `bot.place_limit(coin, side, price, size, opts)` | Place a limit order |
| `bot.place_trigger(coin, side, trigger_px, size, opts)` | Place a trigger (stop) order |
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
| `bot.save_state(key, value)` | Save persistent state |
| `bot.load_state(key)` | Load persistent state |
| `bot.log(level, message)` | Log a message |
| `bot.time()` | Current timestamp (ms) |

### Hot-Reload

Lua files are monitored every 5 seconds. Any modification is detected and the `lua_State` is transparently swapped — no bot restart required.

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

- **Emergency close**: all positions closed at -12 USDC (before the daily limit), fires once per session (atomic flag prevents re-entry loops)
- **Daily loss limit**: -15 USDC (hard stop, automatic pause)
- **Maximum leverage**: 5x
- **Maximum position size**: 200 USD
- **Automatic stop loss**: computed pre-trade
- **Capital allocation**: 5 strategies x $40/trade = $200 notional max, $40 margin at 5x (leaves $60 buffer on $100 account)
- **BB Scalping stops**: tight 1.5% SL + 3% TP per scalp, 2h max hold time — SL/TP placed as trigger orders on exchange immediately after entry fill
- **BB regime filter**: skips trades when Bollinger Bands too tight (< 1%) or too wide (> 8%)
- **Position reconciliation**: REST sync every 15 seconds to detect external fills and position changes

## AI Advisory

Claude Haiku is called automatically:
- **On startup** (15 seconds after boot, once market data has arrived)
- **Twice daily** at 8:00 and 20:00 UTC

Each call sends a structured JSON context containing:
- Current positions and P&L
- Macro data (BTC, dominance, Gold, S&P500, DXY)
- Crypto news sentiment and Fear & Greed Index
- Trade history from the last 24 hours
- Current strategy parameters

The AI returns JSON adjustments (entry sizing, stop-loss/take-profit percentages, pause/resume, risk parameters) that are automatically applied to strategies via the `on_advisory()` callback. Markdown code fences in the response are stripped before parsing.

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
- Emergency close: positions force-closed with 5% slippage tolerance (IOC limit), atomic flag prevents re-entry
- Engine startup: cleanup on partial failure (goto-based resource release)
- Restrictive file permissions: database `0600`, logs `0600`, directories `0700`
- Path traversal protection on strategy file loading
- Rate limiting on Hyperliquid REST API calls (1200 requests/minute)

## Recommended Deployment

1. **Tests**: `ctest --test-dir build`
2. **Backtest**: validate strategies on 30+ days of historical data
3. **Paper trading**: minimum 7 days, verify stability
4. **Testnet**: 3 days on the Hyperliquid testnet (`is_testnet: true` in configuration)
5. **Live (reduced budget)**: 50 USDC, 48-hour monitoring
6. **Live (full budget)**: 100 USDC

## License

Personal use only.
