# Prompt IA : Recoder le Trading Bot en Python

Tu es un ingenieur logiciel expert. Tu dois recoder un bot de trading algorithmique actuellement ecrit en C (engine) + Lua (strategies) + Electron/React (GUI) en **Python pur** (backend + strategies). Le GUI Electron/React reste inchange ou est adapte en webapp Flask/FastAPI si necessaire.

Ce document est la specification COMPLETE. Implemente tout sans rien inventer. Quand une valeur, un seuil, un nom de champ ou un protocole est specifie, utilise-le **exactement**.

---

## REGLES DE CODE

### Style
- Code concis : pas de commentaires evidents, pas de docstrings inutiles. Un commentaire uniquement si la logique n'est pas evidente.
- Noms explicites : variables, fonctions, classes doivent se lire comme du texte. Pas d'abreviations sauf conventions etablies (oid, pnl, tp, sl, atr, rsi...).
- Pas de sur-ingenierie : pas de factory, pas d'abstract base inutile, pas de generics quand un type concret suffit. Pas de design pattern pour le plaisir.
- DRY mais pas premature : 3 lignes identiques = OK. Abstraction uniquement si la duplication est reelle et stable.
- Fonctions courtes : max ~40 lignes. Si plus, decouper.
- Pas de code mort, pas de TODOs, pas de FIXME laisses en place.
- Imports explicites, pas de `from x import *`.
- Type hints sur les signatures publiques. Pas besoin sur les variables locales evidentes.
- Dataclasses ou NamedTuples pour les structs. Pas de dicts bruts pour des donnees structurees.
- f-strings, pas de `.format()` ni `%`.
- `pathlib.Path` pour les chemins fichiers.
- Logging via `logging` module, pas de `print()` en production.

### Architecture
- Modules plats : un fichier par responsabilite. Pas de hierarchie profonde.
- Pas de singleton pattern : passer les dependances en parametres.
- Separation stricte : l'engine ne connait pas les strategies. Les strategies ne connaissent pas l'exchange. Tout passe par des interfaces (protocoles Python).
- Async : utiliser `asyncio` pour le WebSocket et les timers. Le REST peut etre sync (httpx) ou async.
- Threads : uniquement si necessaire pour le dashboard terminal. Le Lua est remplace par du Python natif (pas de sandbox).

---

## REGLES DE SECURITE

### Obligatoires (appliquer systematiquement)
1. **Secrets** : jamais hardcodes. Variables d'environnement via `os.getenv()`. Wipe apres usage si possible. `.env` dans `.gitignore`.
2. **Injection SQL** : requetes parametrees uniquement (`?` placeholders). JAMAIS de f-string dans une requete SQL.
3. **Validation des entrees** : tout input externe (JSON config, API response, user input) est valide et sanitise avant usage.
4. **Pas de `eval()`, `exec()`, `pickle.loads()`** sur des donnees externes.
5. **HTTPS uniquement** : toutes les connexions reseau utilisent TLS. Verifier les certificats (`verify=True`).
6. **Rate limiting** : respecter les limites API (1200 req/min Hyperliquid, 200ms entre requetes Binance).
7. **Erreurs** : ne jamais exposer de stack traces, secrets, ou chemins internes dans les logs de production.
8. **Fichiers** : valider les chemins (pas de path traversal). `Path.resolve()` avant tout acces.
9. **Deps** : utiliser uniquement des packages maintenus et largement utilises. Verifier les CVE.
10. **Cle privee** : la cle EIP-712 est chargee, utilisee, puis effacee de la memoire. Jamais loguee.

### Exchange-specifiques
- **EIP-712 signing** : secp256k1 + keccak256. La signature doit etre reproductible bit-a-bit avec l'implementation C.
- **Nonce** : millisecondes epoch, big-endian 8 bytes.
- **Source** : `"a"` (mainnet) ou `"b"` (testnet) — type STRING, pas ADDRESS.
- **Vault flag** : `0x00` (pas vault) ou `0x01` + 20 bytes address.
- **Trigger orders** : le child OID differe du parent OID. Prevoir un fallback par coin + closed_pnl.

---

## ARCHITECTURE GLOBALE

```
trading_bot/
  engine.py              # Lifecycle : create/start/stop/destroy, wiring
  config.py              # JSON config + .env secrets
  types.py               # Dataclasses : Order, Position, Fill, Candle, etc.
  decimal_utils.py       # Fixed-point (mantissa/scale) pour les prix Hyperliquid
  db.py                  # SQLite wrapper (WAL mode, prepared statements)
  logging_config.py      # Setup logging

  exchange/
    rest.py              # Hyperliquid REST (/info, /exchange)
    ws.py                # WebSocket (allMids, fills, orders, candles, l2Book)
    signing.py           # EIP-712 secp256k1+keccak256
    order_manager.py     # Order lifecycle, OID tracking, fill routing
    paper_exchange.py    # Paper trading simulation (isolated per strategy)

  strategy/
    base.py              # Protocol/ABC pour les strategies
    loader.py            # Chargement dynamique, hot-reload, COIN injection
    indicators.py        # 18+ indicateurs techniques
    api.py               # Fonctions exposees aux strategies (get_indicators, place_limit, etc.)

  risk/
    risk_manager.py      # Pre-trade checks, daily loss, circuit breaker

  backtest/
    engine.py            # 5m simulation, TF aggregation, order fills
    runner.py            # CLI backtest_json equivalent, JSON output
    monte_carlo.py       # Bootstrap simulation 10k runs

  data/
    data_manager.py      # Fear&Greed, sentiment

  report/
    dashboard.py         # Terminal ANSI dashboard

  tools/
    candle_fetcher.py    # Download 5m candles Binance → SQLite
    funding_fetcher.py   # Download funding rates Binance → SQLite
    signal_scanner.py    # 65 signals × combos, walk-forward
    regime_analyzer.py   # Per-regime analysis, Monte Carlo

  strategies/            # Fichiers de strategie (Python natif, pas Lua)
    btc_sniper_1h.py
    doge_sniper_relaxed_1h.py
    sol_range_breakout_1h.py
    template.py

  config/
    bot_config.json

  data/                  # SQLite DBs, backtest results
  logs/
  scripts/               # start.sh, grid_search.sh, etc.
```

---

## TYPES DE DONNEES (types.py)

```python
from dataclasses import dataclass, field
from enum import Enum, auto

class Side(Enum):
    BUY = "buy"
    SELL = "sell"

class TIF(Enum):
    GTC = "gtc"
    IOC = "ioc"
    ALO = "alo"   # Post-only (maker)

class OrderType(Enum):
    LIMIT = "limit"
    TRIGGER = "trigger"  # SL/TP

class TPSL(Enum):
    TP = "tp"
    SL = "sl"

class Grouping(Enum):
    NA = 0
    NORMAL_TPSL = 1
    POS_TPSL = 2

@dataclass
class Decimal:
    mantissa: int
    scale: int  # value = mantissa / 10^scale

    def to_float(self) -> float: ...
    def to_str(self) -> str: ...

    @staticmethod
    def from_float(value: float, scale: int = 8) -> "Decimal": ...

@dataclass
class OrderRequest:
    asset: int
    coin: str            # "BTC"
    side: Side
    price: Decimal
    size: Decimal
    reduce_only: bool = False
    order_type: OrderType = OrderType.LIMIT
    tif: TIF = TIF.ALO
    is_market: bool = False
    trigger_px: Decimal | None = None
    tpsl: TPSL | None = None
    cloid: str = ""
    grouping: Grouping = Grouping.NA

@dataclass
class Order:
    oid: int
    asset: int
    coin: str
    side: Side
    limit_px: Decimal
    sz: Decimal
    orig_sz: Decimal
    timestamp_ms: int
    reduce_only: bool = False
    tif: TIF = TIF.GTC
    cloid: str = ""

@dataclass
class Fill:
    coin: str
    px: Decimal
    sz: Decimal
    side: Side
    time_ms: int
    closed_pnl: Decimal      # 0 pour les entries
    fee: Decimal
    oid: int
    tid: int
    crossed: bool = False     # True = taker
    hash: str = ""

@dataclass
class Position:
    coin: str
    asset: int = 0
    size: Decimal = field(default_factory=lambda: Decimal(0, 8))  # negatif = short
    entry_px: Decimal = field(default_factory=lambda: Decimal(0, 8))
    unrealized_pnl: Decimal = field(default_factory=lambda: Decimal(0, 8))
    realized_pnl: Decimal = field(default_factory=lambda: Decimal(0, 8))
    liquidation_px: Decimal = field(default_factory=lambda: Decimal(0, 8))
    margin_used: Decimal = field(default_factory=lambda: Decimal(0, 8))
    leverage: int = 1
    is_cross: bool = False

@dataclass
class Candle:
    time_open: int       # ms
    time_close: int      # ms
    open: float
    high: float
    low: float
    close: float
    volume: float
    n_trades: int = 0

@dataclass
class BookLevel:
    px: Decimal
    sz: Decimal
    n_orders: int = 0

@dataclass
class Book:
    coin: str
    bids: list[BookLevel]   # max 20 levels
    asks: list[BookLevel]
    timestamp_ms: int = 0

@dataclass
class Mid:
    coin: str
    mid: Decimal

@dataclass
class Account:
    account_value: Decimal
    total_margin_used: Decimal
    total_unrealized_pnl: Decimal
    withdrawable: Decimal
    positions: list[Position]

@dataclass
class AssetMeta:
    name: str           # "BTC"
    asset_id: int
    sz_decimals: int    # 8 pour BTC

@dataclass
class AssetCtx:
    coin: str
    funding_rate: float
    premium: float
    open_interest: float
    mark_px: float
    valid: bool = False
```

---

## CONFIG (config.py)

### Fichier JSON : `config/bot_config.json`
```json
{
  "exchange": {
    "rest_url": "https://api.hyperliquid.xyz",
    "ws_url": "wss://api.hyperliquid.xyz/ws",
    "is_testnet": false
  },
  "risk": {
    "daily_loss_pct": 6,
    "emergency_close_pct": 5,
    "max_leverage": 10,
    "max_position_pct": 700
  },
  "strategies": {
    "dir": "./strategies",
    "reload_interval_sec": 5,
    "active": [
      {
        "file": "btc_sniper_1h.py",
        "role": "primary",
        "coins": ["BTC"]
      },
      {
        "file": "doge_sniper_relaxed_1h.py",
        "coins": ["DOGE"],
        "paper_mode": false
      },
      {
        "file": "sol_range_breakout_1h.py",
        "coins": ["SOL"],
        "paper_mode": true,
        "paper_balance": 500
      }
    ]
  },
  "database": {"path": "./data/trading_bot.db"},
  "logging": {"dir": "./logs", "level": 0},
  "mode": {
    "paper_trading": false,
    "paper_initial_balance": 500
  }
}
```

### Secrets (`.env`, jamais commites)
```
TB_PRIVATE_KEY=0x...        # 64 hex chars
TB_WALLET_ADDRESS=0x...     # Checksummed ETH address
```

### Regles de config
- **1 coin = 1 strategy** : refuse de demarrer si un coin est assigne a 2 strategies.
- **Paper mode** : global OU per-strategy. Per-strategy override le global. Chaque strategy paper a son exchange isole (balance, positions, ordres independants).
- **Risk clamp** : leverage max clamp [1, 50], daily_loss clamp [1, 50]%, position_pct clamp [10, 10000]%.
- **Hot-reload** : check mtime toutes les `reload_interval_sec` secondes.

---

## ENGINE (engine.py)

### Lifecycle
```
create():
  1. Load config (JSON + .env)
  2. Validate coin exclusivity
  3. Open SQLite (WAL mode)

start():
  1. Create signer (secp256k1) — skip if paper-only
  2. Create REST client (rate limiter 1200/min)
  3. Create WebSocket — connect, subscribe allMids
  4. Create paper exchanges (global + per-strategy)
  5. Create position tracker (cumulative PnL from DB)
  6. Create risk manager (linked to position tracker)
  7. Create order manager (routes paper vs live)
  8. Load strategies (hot-reload, COIN injection)
  9. Start data manager (Fear&Greed, sentiment)
  10. Start dashboard (terminal, optional)
  11. Wire WS callbacks → engine → strategies
  12. Start timer (periodic on_timer)

stop():
  1. Stop timer
  2. Stop WebSocket
  3. Call on_shutdown() on all strategies
  4. Flush trades to DB
  5. Destroy paper exchanges
  6. Close DB
```

### Threads/Async
- **WebSocket** : asyncio task. Messages → callbacks → engine dispatch.
- **Timer** : asyncio task, appelle `on_timer()` toutes les 60s.
- **REST** : sync ou async (httpx). Rate limited.
- **Strategies** : execution sequentielle (pas de parallelisme dans les callbacks).
- **Dashboard** : thread separe si terminal ANSI.

---

## EXCHANGE REST (exchange/rest.py)

### Endpoints /info (POST, pas d'auth)

| Methode | Request body | Response |
|---------|-------------|----------|
| `get_meta()` | `{"type": "meta"}` | `[{name, szDecimals, assetId}]` |
| `get_all_mids()` | `{"type": "allMids"}` | `{coin: mid_str}` |
| `get_asset_ctxs()` | `{"type": "metaAndAssetCtxs"}` | `[{funding, openInterest, markPx}]` |
| `get_l2_book(coin)` | `{"type": "l2Book", "coin": coin}` | `{levels: [[px, sz, n]]}` |
| `get_candles(coin, interval, start_ms, end_ms)` | `{"type": "candleSnapshot", "req": {coin, interval, startTime, endTime}}` | `[{t, T, o, h, l, c, v, n}]` |
| `get_account(addr)` | `{"type": "clearinghouseState", "user": addr}` | `{marginSummary, assetPositions}` |
| `get_open_orders(addr)` | `{"type": "openOrders", "user": addr}` | `[{oid, coin, side, limitPx, sz, ...}]` |
| `get_user_fills(addr)` | `{"type": "userFills", "user": addr}` | `[{coin, px, sz, side, time, closedPnl, fee, oid, tid, ...}]` |

### Endpoints /exchange (POST, EIP-712 auth)

| Methode | Action |
|---------|--------|
| `place_order(order_req)` | Place un ordre. Retourne OID. |
| `place_orders(order_reqs)` | Place N ordres atomiquement. |
| `cancel_order(asset, oid)` | Cancel un ordre. |
| `cancel_orders([(asset, oid)])` | Cancel N ordres. |
| `update_leverage(asset, leverage, is_cross)` | Change levier. |

### Format de requete /exchange
```json
{
  "action": {
    "type": "order",
    "orders": [{
      "a": asset_id,
      "b": true,          // isBuy
      "p": "40000.0",     // price string
      "s": "0.001",       // size string
      "r": false,         // reduceOnly
      "t": {"limit": {"tif": "Alo"}},
      "c": "0x..."        // cloid (optional)
    }],
    "grouping": "na"
  },
  "nonce": 1710000000000,
  "signature": {"r": "0x...", "s": "0x...", "v": 27},
  "vaultAddress": null
}
```

### Rate Limiting
- **1200 requetes/minute** max.
- Token bucket : refill en fin de minute.
- Backoff adaptatif : 100ms → 2000ms si limite atteinte.

---

## EXCHANGE WEBSOCKET (exchange/ws.py)

### Connexion
- URL : `wss://api.hyperliquid.xyz/ws`
- Librairie : `websockets` (asyncio)
- Reconnexion : backoff exponentiel 100ms → 1s → 10s → max 60s
- Auto-resubscription : sauvegarder les subscriptions, rejouer apres reconnexion

### Subscriptions

```json
// allMids — tous les mid prices
{"method": "subscribe", "subscription": {"type": "allMids"}}

// l2Book — carnet d'ordres L2
{"method": "subscribe", "subscription": {"type": "l2Book", "coin": "BTC"}}

// candle — chandeliers
{"method": "subscribe", "subscription": {"type": "candle", "coin": "BTC", "interval": "1h"}}

// orderUpdates — mises a jour ordres
{"method": "subscribe", "subscription": {"type": "orderUpdates", "user": "0x..."}}

// userFills — fills utilisateur
{"method": "subscribe", "subscription": {"type": "userFills", "user": "0x..."}}
```

### Messages recus
```json
{
  "channel": "allMids",
  "data": {"mids": {"BTC": "42000.5", "ETH": "2300.25"}}
}
```

### Callbacks (vers engine)
- `on_mids(mids: list[Mid])`
- `on_book(book: Book)`
- `on_candle(coin: str, candle: Candle)`
- `on_order_update(orders: list[Order])`
- `on_fill(fills: list[Fill])`

---

## EIP-712 SIGNING (exchange/signing.py)

### Dependances
- `eth_account` ou `py_ecc` pour secp256k1
- `pysha3` ou `eth_hash` pour keccak256

### Flow de signature

```
1. Construire connectionId :
   data = msgpack(action) + nonce.to_bytes(8, "big")
   if vault_address: data += bytes(0x01) + vault_address_bytes
   else: data += bytes(0x00)
   connectionId = keccak256(data)

2. Construire le struct Agent :
   source = "a" (mainnet) ou "b" (testnet)  # TYPE STRING, pas address
   connectionId = hash ci-dessus

3. EIP-712 encode :
   AGENT_TYPE_HASH = keccak256("Agent(string source,bytes32 connectionId)")
   source_hash = keccak256(source_bytes)  # keccak du string, pas du bytes
   structHash = keccak256(AGENT_TYPE_HASH + source_hash + connectionId)

4. Domain separator :
   DOMAIN_TYPE_HASH = keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")
   domainSeparator = keccak256(
     DOMAIN_TYPE_HASH +
     keccak256("Exchange") +
     keccak256("1") +
     uint256(1337) +
     address(0x0000...0000)
   )

5. Digest :
   digest = keccak256(0x1901 + domainSeparator + structHash)

6. Signer :
   (v, r, s) = secp256k1_sign_recoverable(digest, private_key)
   v = 27 ou 28
```

### CRITICAL
- **Nonce** = `int(time.time() * 1000)` (millisecondes)
- **Source** = string `"a"` (mainnet), `"b"` (testnet). PAS une address.
- **keccak256("a")** pour le encoding EIP-712 du string.
- **Msgpack** : action serialisee en msgpack avant hash.

---

## ORDER MANAGER (exchange/order_manager.py)

### Responsabilites
1. **Routing** : paper vs live (global paper OU per-strategy paper OU live)
2. **Risk check** pre-trade via risk_manager
3. **OID tracking** : cache `oid → strategy` dans SQLite
4. **Fill handling** : dispatcher fills vers la bonne strategy
5. **Reconciliation** : thread/task REST toutes les 30s pour syncer les ordres

### Logique fills
- `closed_pnl == 0` → fill d'ENTREE (y compris partiels)
- `closed_pnl != 0` → fill de SORTIE
- **Partial fills** : chaque partial est un event separe. SL/TP refreshes a chaque partial (cancel + replace avec taille reelle).
- **Trigger OID mismatch** : quand un trigger fire, Hyperliquid cree un child order avec un NOUVEL OID. Le fill porte le child OID → lookup direct echoue → fallback par coin + `closed_pnl != 0`.

### Cache OID → Strategy
```sql
CREATE TABLE order_strategy_map (
    oid INTEGER PRIMARY KEY,
    strategy TEXT NOT NULL,
    coin TEXT NOT NULL,
    created_ms INTEGER NOT NULL
);
```
Buffer circulaire de 2048 entries. Cleanup periodique des entries > 24h.

---

## PAPER EXCHANGE (exchange/paper_exchange.py)

### Principe
- Exchange simule entierement en memoire.
- **Un par strategy** en mode paper per-strategy (balance, positions, ordres isoles).
- **Un global** en mode paper global.
- Matching sur mid price (pas de carnet d'ordres simule).

### Fees
- Taker : 0.045% (0.00045)
- Maker : 0.015% (0.00015)
- Trigger orders = taker fee

### Matching (`feed_mid(coin, price)`)
```
Pour chaque ordre actif sur ce coin :
  Si limit order :
    BUY  : fill si price <= limit_px
    SELL : fill si price >= limit_px
  Si trigger order :
    TP BUY (short exit)  : fill si price <= trigger_px
    TP SELL (long exit)   : fill si price >= trigger_px  # NON — inverse
    SL BUY (short exit)  : fill si price >= trigger_px
    SL SELL (long exit)   : fill si price <= trigger_px
  Fill → update position, deduct fee, callback
```

### Position tracking
- Entry : `entry_px` calcule en moyenne ponderee si partiels
- Exit : `realized_pnl = (exit_px - entry_px) * size` (ajuste pour le side)
- Unrealized : recalcule a chaque `feed_mid`

---

## STRATEGIES (strategy/)

### Interface (base.py)
```python
from typing import Protocol

class Strategy(Protocol):
    name: str
    coin: str

    def on_init(self, api: StrategyAPI) -> None: ...
    def on_tick(self, coin: str, mid_price: float) -> None: ...
    def on_fill(self, fill: Fill) -> None: ...
    def on_timer(self) -> None: ...
    def on_book(self, book: Book) -> None: ...
    def on_shutdown(self) -> None: ...
```

### API exposee (api.py → classe StrategyAPI)

| Methode | Signature | Description |
|---------|-----------|-------------|
| `place_limit` | `(coin, side, price, size, tif="alo", reduce_only=False) → oid` | Ordre limit |
| `place_trigger` | `(coin, side, price, size, trigger_px, tpsl) → oid` | Ordre SL/TP |
| `cancel` | `(coin, oid)` | Cancel ordre |
| `cancel_all` | `(coin)` | Cancel tous ordres du coin |
| `get_position` | `(coin) → Position or None` | Position courante |
| `get_mid_price` | `(coin) → float or None` | Mid price WS |
| `get_open_orders` | `(coin) → list[Order]` | Ordres ouverts |
| `get_candles` | `(coin, interval, count, live_price=None) → list[Candle]` | Chandeliers (cache 30s, 8 slots) |
| `get_indicators` | `(coin, interval, count, live_price=None) → Indicators` | Snapshot indicateurs |
| `get_account_value` | `() → float` | Equity totale |
| `get_daily_pnl` | `() → float` | PnL du jour (reset UTC midnight) |
| `save_state` | `(key, value)` | Persistence SQLite |
| `load_state` | `(key) → value or None` | Restore depuis SQLite |
| `log` | `(level, msg)` | Log via logging module |
| `time` | `() → float` | Epoch secondes (float) |
| `get_funding_rate` | `(coin) → FundingRate or None` | Funding rate (live: 60s cache, backtest: historique SQLite) |
| `get_open_interest` | `(coin) → float or None` | OI (live only, None en backtest) |

### Cache candles
- 8 slots (LRU eviction)
- 30 secondes TTL
- Max 300 candles par slot
- Cle : `(coin, interval)`

---

## INDICATEURS (strategy/indicators.py)

### Snapshot complet (dataclass `Indicators`)

Tous les champs ci-dessous doivent etre calcules. `valid = True` si `n_candles >= 200`.

**Moyennes mobiles** : `sma_20`, `sma_50`, `sma_200`, `ema_12`, `ema_26`

**RSI(14)** : `rsi_14`. RS = avg_gain / avg_loss. RSI = 100 - 100/(1+RS). Smoothed (Wilder).

**MACD(12,26,9)** : `macd_line` (EMA12 - EMA26), `macd_signal` (EMA9 de macd_line), `macd_histogram` (line - signal).

**Bollinger Bands(20, 2.0)** : `bb_upper`, `bb_middle`, `bb_lower`, `bb_width` (upper-lower)/middle.

**ATR(14)** : `atr_14`. True Range = max(H-L, |H-prevC|, |L-prevC|). EMA smoothing (Wilder).

**VWAP** : `vwap`. Cumulative (typical_price * volume) / cumulative_volume.

**ADX(14)** : `adx_14`, `plus_di`, `minus_di`. Smoothed DI (Wilder).

**Keltner Channels(20, 14, 1.5)** : `kc_upper/middle/lower`. middle=EMA20, bands=EMA ± 1.5*ATR14.

**Donchian Channels(20)** : `dc_upper/lower/middle`. upper=highest high 20 bars, lower=lowest low.

**Stochastic RSI(14, 14, 3, 3)** : `stoch_rsi_k`, `stoch_rsi_d`.

**CCI(20)** : `cci_20`. (typical - SMA) / (0.015 * mean_deviation).

**Williams %R(14)** : `williams_r`. Range [-100, 0].

**OBV** : `obv`, `obv_sma` (SMA20 de OBV).

**Ichimoku** : `ichi_tenkan` (9H+9L)/2, `ichi_kijun` (26H+26L)/2, `ichi_senkou_a` (tenkan+kijun)/2 shifted 26, `ichi_senkou_b` (52H+52L)/2 shifted 26, `ichi_chikou` = close shifted back 26.

**CMF(20)** : `cmf_20`. MF multiplier = ((C-L)-(H-C))/(H-L). CMF = sum(MF*V) / sum(V).

**MFI(14)** : `mfi_14`. Range [0, 100]. Comme RSI mais avec volume.

**Squeeze Momentum** : `squeeze_mom`, `squeeze_on` (bool). squeeze_on = BB inside KC. mom = close - average(KC_mid, DC_mid).

**ROC(12)** : `roc_12`. (close - close[12]) / close[12] * 100.

**Z-Score(20)** : `zscore_20`. (close - SMA20) / stddev20.

**FVG** : `fvg_bull` (bool), `fvg_bear` (bool), `fvg_size` (% du prix). Bullish FVG : candle[i-2].high < candle[i].low.

**Supertrend(10, 3.0)** : `supertrend` (value), `supertrend_up` (bool).

**Parabolic SAR(0.02, 0.20, 0.02)** : `psar` (value), `psar_up` (bool).

**Signaux derives** :
- `above_sma_200`, `golden_cross` (SMA50 > SMA200), `rsi_oversold` (<30), `rsi_overbought` (>70)
- `bb_squeeze` (width < 3%), `macd_bullish_cross`, `adx_trending` (>25), `kc_squeeze`
- `ichi_bullish` (price > cloud + tenkan > kijun)
- `atr_pct_rank`, `range_pct_rank` (percentile sur 100 barres)
- `ema12_dist_pct`, `sma20_dist_pct` (distance % au prix)
- `vol_ratio` (volume / SMA20_volume), `atr_pct` (ATR/close)
- `consec_green`, `consec_red` (compteur bougies consecutives)
- `bullish_engulf`, `bearish_engulf`, `shooting_star`, `hammer`, `doji`
- `macd_hist_incr`, `macd_hist_decr`
- `di_bull` (+DI > -DI), `di_bear` (-DI > +DI)
- `rsi_bull_div`, `rsi_bear_div`

### Aliases (compatibilite)
- `sma` = `sma_20`
- `ema` = `ema_fast` = `ema_12`
- `ema_slow` = `ema_26`
- `bb_mid` = `bb_middle`

---

## RISK MANAGER (risk/risk_manager.py)

### Pre-trade check (`check_order()`)
Ordre des verifications :
1. **Paused?** → reject
2. **Daily loss** : `|daily_pnl| > account_value * daily_loss_pct / 100` → reject
3. **Leverage** : notional de l'ordre / equity > max_leverage → reject
4. **Position size** : notional > account_value * max_position_pct / 100 → reject
5. **Global exposure** : somme notionals de toutes les positions → reject si > max

### Emergency close
- `daily_pnl > account_value * emergency_close_pct / 100` → force close toutes les positions

### Circuit breaker
- Fenetre glissante 15 minutes
- Si mouvement prix > 7% → bloque nouvelles entries
- SL/TP/gestion de position restent autorises

### Daily reset
- A UTC midnight : reset daily_pnl, daily_fees, daily_trades

---

## BACKTEST ENGINE (backtest/engine.py)

### Principe
- Charge des bougies **5m** depuis SQLite
- Agrege en TF natif de la strategie (1h = 12 bougies 5m, 4h = 48)
- `on_tick()` appele uniquement quand une bougie TF se complete
- Fills verifies sur high/low des bougies 5m
- Guard `placed_at_idx` : un ordre place au tick i ne peut pas fill au tick i

### Config backtest
```python
@dataclass
class BacktestConfig:
    coin: str
    strategy_path: str
    initial_balance: float = 100.0
    max_leverage: int = 10
    maker_fee: float = 0.00015
    taker_fee: float = 0.00045
    slippage_bps: float = 1.0
    strategy_interval_ms: int = 3_600_000  # 1h
    grid_tp: float = 0.0        # override TP%
    grid_sl: float = 0.0        # override SL%
```

### Agregation 5m → TF
```
Pour chaque groupe de N bougies 5m (N = interval_ms / 300_000) :
  candle_tf.open   = first_5m.open
  candle_tf.high   = max(all_5m.high)
  candle_tf.low    = min(all_5m.low)
  candle_tf.close  = last_5m.close
  candle_tf.volume = sum(all_5m.volume)
  candle_tf.time   = first_5m.time_open
```

### Simulation fills
```
Pour chaque bougie 5m :
  Pour chaque ordre actif (si placed_at_idx != current_idx) :
    Si limit BUY et low <= limit_px → fill
    Si limit SELL et high >= limit_px → fill
    Si trigger TP (long exit) et high >= trigger_px → fill
    Si trigger SL (long exit) et low <= trigger_px → fill
    Si trigger TP (short exit) et low <= trigger_px → fill
    Si trigger SL (short exit) et high >= trigger_px → fill
```

### Ordre des operations par bougie 5m
1. Check fills pass 1 (limit orders)
2. Check fills pass 2 (trigger orders)
3. Si bougie TF complete : agreger, appeler `on_tick()`
4. Mettre a jour equity curve

### Trade log
```python
@dataclass
class BacktestTrade:
    time_ms: int
    side: str
    price: float
    size: float
    pnl: float
    fee: float
    balance_after: float
```

### Resultats
```python
@dataclass
class BacktestResult:
    start_balance: float
    end_balance: float
    total_pnl: float
    total_fees: float
    return_pct: float
    total_trades: int
    winning_trades: int
    losing_trades: int
    win_rate: float
    profit_factor: float
    avg_win: float
    avg_loss: float
    max_win: float
    max_loss: float
    max_drawdown_pct: float
    sharpe_ratio: float       # sqrt(252) * mean/std
    sortino_ratio: float      # sqrt(252) * mean/downside_std
    trades: list[BacktestTrade]
    equity_curve: list[dict]  # {time_ms, equity, drawdown}
```

### Funding rates en backtest
- Charges depuis table `funding_rates` dans SQLite
- Recherche binaire par timestamp (O(log n))
- `get_funding_rate()` retourne le rate le plus recent avant le tick courant
- `premium` = 0 en backtest (pas dispo historiquement)
- Necessite `funding_fetcher` pre-run

### Monte Carlo (backtest/monte_carlo.py)
- PRNG : xoshiro256** (seed 42, deterministe)
- 10,000 simulations
- Bootstrap : sampling avec remplacement des PnL par trade
- Outputs : `p_ruin_50pct`, `p95_drawdown`, `p99_drawdown`, `median_return`, `p5_return`, `p95_return`, `final_equity_median`
- Integre dans la sortie JSON du backtest si >= 5 trades

---

## STRATEGIES PRODUCTION

### Pattern universel
```python
class MyStrategy:
    def __init__(self):
        self.coin = ""  # Injecte par le loader
        self.api = None  # StrategyAPI, injecte par le loader

        # Position state
        self.in_position = False
        self.position_side = ""
        self.entry_price = 0.0
        self.entry_time = 0.0
        self.entry_oid = 0
        self.entry_placed_at = 0.0
        self.sl_oid = 0
        self.tp_oid = 0

        # Stats
        self.trade_count = 0
        self.win_count = 0
        self.consec_losses = 0
        self.peak_equity = 0.0

        # Timing
        self.last_check = 0.0
        self.last_trade = 0.0

        # MACD history (pour deceleration)
        self.last_hour = 0
        self.last_macd_val = 0.0
        self.prev_macd = 0.0
        self.prev2_macd = 0.0

    def on_init(self, api):
        self.api = api
        self.coin = api.coin
        # Restore state from DB
        # Check existing position

    def on_tick(self, coin, mid_price):
        if coin != self.coin: return
        now = self.api.time()
        if now - self.last_check < self.check_sec: return
        self.last_check = now

        if self.in_position:
            self._monitor_position(mid_price, now)
            return

        if now - self.last_trade < self.cooldown_sec: return

        ind = self.api.get_indicators(self.coin, "1h", 50, mid_price)
        if not ind: return

        signal = self._scan_signals(ind, mid_price)
        if signal:
            self._place_entry(signal, mid_price, now)

    def on_fill(self, fill):
        if fill.closed_pnl == 0:
            self._handle_entry_fill(fill)
        else:
            self._handle_exit_fill(fill)

    def on_timer(self):
        # Timeout entries, sync position, check pauses

    def on_shutdown(self):
        # Persist state
```

### BTC Sniper 1h
- **Config** : equity 50%, leverage 7x, cooldown 4h, max_hold 48h
- **Signaux** :
  - L1 (LONG) : RSI > 65 AND ATR/mid < 0.004 AND MACD deceleration (histogram < prev < prev2)
  - S1 (SHORT) : RSI < 30 AND MACD < 0 AND ATR/mid < 0.004
- **TP/SL** : 2.0% / 2.0%
- **Entry** : ALO limit 0.02% du mid, timeout 90s
- **Sizing** : equity * leverage * drawdown_mult
  - 3+ consec losses → 25%, 2 consec → 50%
  - Peak DD > 20% → stop, > 15% → 25%, > 10% → 50%

### DOGE Sniper Relaxed 1h
- **Config** : equity 40%, leverage 5x, cooldown 4h, max_hold 48h, ATR threshold 0.9%
- **Signaux** :
  - L1 (LONG) : RSI > 65 AND ATR/mid < 0.009 AND MACD decel
  - L3 (LONG) : RSI > 65 AND ADX > 20 AND ATR/mid < 0.009
  - S1 (SHORT) : RSI < 35 AND MACD < 0 AND ATR/mid < 0.009 (signal DOMINANT)
  - S2 (SHORT) : RSI < 35 AND ATR/mid < 0.009 AND (SMA20 < SMA50 OR MACD < 0)
- **TP/SL** : 4.5% / 1.5% (RR 3:1), overridable via GRID_TP/GRID_SL
- **ATR-adaptive sizing** : mult = baseline(0.006) / current_atr, clamp [0.5, 1.5]
- **Trailing stop** (code present, desactive) : activate a +3.5%, offset 1.0%, step 0.5%
- **Pause** : 4 consec losses → pause 8h
- **DD guards** : total DD > 29% → stop, monthly > 25% → stop, weekly > 15% → 25%, daily > 7% → 50%

### SOL Range Breakout 1h
- **Config** : equity 40%, leverage 5x, cooldown 4h, max_hold 48h
- **Mode 1 — Range Breakout (tous regimes)** :
  - Lookback 24 barres, range [0.5%, 4%], volume > 1.2x moyenne
  - LONG si close > range_hi, SHORT si close < range_lo
  - TP/SL : 4.5% / 1.5%
- **Mode 2 — Bear B1 (SHORT only, regime bear)** :
  - Gate bear : close < SMA200 AND SMA50 < SMA200 AND ADX > 20
  - Signal : RSI < 35 AND MACD < 0 AND ATR/mid < 1.2%
  - TP/SL : 6.0% / 2.0% (RR 3:1)
- **Priorite** : B1 teste AVANT RB
- **ATR-adaptive sizing** : baseline 0.8%, clamp [0.5, 1.5]
- **DD guards** : total DD > 34% → stop, monthly > 25% → stop, weekly > 18% → 25%, daily > 8% → 50%

---

## SIGNAL SCANNER (tools/signal_scanner.py)

### 65 signaux booleens

**RSI** : rsi>65, rsi>70, rsi<35, rsi<30, rsi_40_60
**Volatilite** : low_vol (ATR/close<0.5%), very_low_vol (<0.3%), high_vol (>1%), atr_p80, atr_p20
**ADX** : adx>25, adx<20, adx>30
**DI** : di_bull (+DI>-DI), di_bear (-DI>+DI)
**MACD** : macd>0, macd<0, macd_accel, macd_decel, macd_bull_cross
**Bollinger** : bb_squeeze (width<3%), above_bb_upper, below_bb_lower, below_bb_mid
**SMA** : above_sma200, below_sma200, golden_cross, death_cross
**SMA distance** : sma20_far_above (2%+), sma20_far_below (2%+)
**EMA** : ema12_far_above, ema12_far_below, ema_up (12>26), ema_down (12<26)
**Volume** : vol_spike_2x, vol_spike_3x, low_volume (<50% SMA20)
**OBV** : obv_above_sma, obv_below_sma
**MFI** : mfi>70, mfi<20
**CMF** : cmf>0.5, cmf<-0.5
**CCI** : cci>100, cci<-100
**Candle patterns** : bullish_engulf, bearish_engulf, hammer, shooting_star, doji
**Ichimoku** : ichi_bullish, ichi_bearish
**Divergence** : rsi_bull_div, rsi_bear_div
**Squeeze** : squeeze_on, squeeze_off

### Scanning combos
```
Pour max_combo in [1, 2, 3] :
  Pour chaque combinaison de N signaux :
    Pour TP in [1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 6.0] :
      Pour SL in [0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0] :
        Simuler single-position sequentiel :
          IS (70%) : WR, PF, Sharpe, EV
          OOS (30%) : WR, PF, Sharpe, EV
        Sauvegarder si OOS Sharpe > 0

Output : TSV trie par OOS Sharpe, top 500
```

### Walk-forward
- IS : 70% des bougies (anciennes)
- OOS : 30% des bougies (recentes)
- Min 20 occurrences par signal combo

---

## REGIME ANALYZER (tools/regime_analyzer.py)

### Classification regime
```
BULL    : SMA50 > SMA200 AND EMA12 > EMA26 AND (ADX > 20 OR +DI > -DI)
BEAR    : SMA50 < SMA200 AND EMA12 < EMA26 AND (ADX > 20 OR -DI > +DI)
NEUTRAL : tout le reste
```
- **Hysteresis** : 3 bougies consecutives dans le nouveau regime avant transition
- **Matrice de transition** : compter les transitions entre regimes

### Walk-forward 5-split
- 5 fenetres glissantes, 60% overlap
- Train 70% / Test 30% par fenetre
- Score robustesse : degradation IS→OOS < 30% = "ROBUST"
- Decay weighting : fenetres recentes comptent plus

### Monte Carlo
- xoshiro256** (seed 42)
- 10,000 simulations bootstrap
- Regime-aware MC : sequence Markov de regimes, trades per-regime
- Outputs : P(ruin 50%), P95 DD, P5/P95 quantiles, median return

### CLI
```
./regime_analyzer ETH 2000 1h [--validate] [--montecarlo]
```

### Rapport → `data/analysis/{COIN}_regime_report.md`

---

## CANDLE FETCHER (tools/candle_fetcher.py)

### Source : Binance Futures API (public)
- Endpoint : `https://fapi.binance.com/fapi/v1/klines`
- Params : `symbol=BTCUSDT&interval=5m&startTime=...&limit=1500`
- Incremental : reprend depuis `MAX(time_open)` en DB
- Rate limiting : 200ms entre requetes
- Pagination : batch 1500, avance par `startTime`

### Schema SQLite
```sql
CREATE TABLE candles (
    coin TEXT NOT NULL,
    interval TEXT NOT NULL,
    time_open INTEGER NOT NULL,
    open REAL, high REAL, low REAL, close REAL,
    volume REAL, n_trades INTEGER,
    PRIMARY KEY (coin, interval, time_open)
);
```

### IMPORTANT : stocker uniquement les bougies 5m. Les TF superieurs sont agrege a la volee.

---

## FUNDING FETCHER (tools/funding_fetcher.py)

### Source : Binance Futures API (public)
- Endpoint : `https://fapi.binance.com/fapi/v1/fundingRate`
- Params : `symbol=BTCUSDT&startTime=...&limit=1000`
- Rate limiting : 200ms entre requetes
- Incremental depuis `MAX(time_ms)`

### Schema SQLite
```sql
CREATE TABLE funding_rates (
    coin TEXT NOT NULL,
    time_ms INTEGER NOT NULL,
    rate REAL NOT NULL,
    mark_price REAL NOT NULL,
    PRIMARY KEY (coin, time_ms)
);
CREATE INDEX idx_fr_lookup ON funding_rates(coin, time_ms);
```

---

## DATABASE (db.py)

### SQLite avec WAL mode
```python
conn = sqlite3.connect(db_path)
conn.execute("PRAGMA journal_mode=WAL")
conn.execute("PRAGMA synchronous=NORMAL")
```

### Tables
```sql
-- Bougies (cache)
candles(coin, interval, time_open, open, high, low, close, volume, n_trades)

-- Funding rates
funding_rates(coin, time_ms, rate, mark_price)

-- Trades executes
trades(id, oid, tid, coin, side, price, size, fee, closed_pnl, strategy, time_ms, hash)

-- Etat Lua/Python persistant (key-value)
strategy_state(strategy TEXT, key TEXT, value TEXT, PRIMARY KEY(strategy, key))

-- Cache OID → strategy
order_strategy_map(oid INTEGER PRIMARY KEY, strategy TEXT, coin TEXT, created_ms INTEGER)
```

### Requetes parametrees UNIQUEMENT
```python
# CORRECT
cursor.execute("SELECT * FROM trades WHERE coin=? AND time_ms>?", (coin, start_ms))

# INTERDIT
cursor.execute(f"SELECT * FROM trades WHERE coin='{coin}'")
```

---

## GUI (si webapp Python)

Si tu remplaces le GUI Electron par une webapp Python :
- **FastAPI** backend + **React** frontend (ou HTMX)
- WebSocket pour les updates temps reel (positions, trades, mid prices)
- Les 10 namespaces IPC deviennent des endpoints REST/WS

| Namespace | → Route |
|-----------|---------|
| bot | `/api/bot/start`, `/api/bot/stop`, `/api/bot/status` |
| config | `/api/config` (GET/PUT) |
| strategies | `/api/strategies` (CRUD) |
| backtest | `/api/backtest/run` (POST, SSE progress) |
| db | `/api/trades`, `/api/positions` |
| market | `/api/market/fear-greed`, `/api/market/candles` |
| logs | `/api/logs` (GET, tail SSE) |
| ws | WebSocket `/ws/live` (mids, fills, orders) |
| sync | `/api/sync/exchange` |
| license | `/api/license/check`, `/api/license/activate` |

---

## LICENCE

### Ed25519
- Cle publique dans l'app (verification uniquement)
- Cle privee admin-only (signature)
- Machine ID : SHA-256(platform UUID) tronque 16 hex
  - macOS : `IOPlatformUUID` via `ioreg`
  - Linux : `/etc/machine-id`
  - Windows : `wmic csproduct get UUID`
- Token : base64url(`{c: code, m: machineId, s: Ed25519_sign("code:machineId")}`)
- 1 code = 1 machine (lie via signature)
- Stockage : `license.dat` chiffre AES-256-GCM avec cle derivee du machineId

---

## FEES HYPERLIQUID

| Type | Fee |
|------|-----|
| Maker (ALO) | 0.0150% (1.5 bps) |
| Taker (market, trigger) | 0.0450% (4.5 bps) |
| **Round-trip** | **0.06%** |

Toujours inclure les fees dans les calculs de PnL, backtest, signal scanning.

---

## CHECKLIST DE VALIDATION

Apres implementation, verifier :

1. [ ] Les indicateurs (RSI, MACD, BB, ATR, ADX, etc.) matchent TA-Lib a < 0.01% d'ecart
2. [ ] Le backtest 5m avec agregation TF produit les memes resultats que l'implementation C
3. [ ] Le signing EIP-712 produit des signatures valides acceptees par Hyperliquid
4. [ ] Les partial fills sont geres correctement (SL/TP refresh)
5. [ ] Le trigger OID mismatch est gere (fallback par coin + closed_pnl)
6. [ ] 1 coin = 1 strategy est enforce au demarrage
7. [ ] Paper exchange isole par strategy fonctionne
8. [ ] Hot-reload des strategies fonctionne (check mtime)
9. [ ] Rate limiting respecte (1200/min REST, 200ms Binance)
10. [ ] Monte Carlo integre dans le backtest JSON output
11. [ ] Walk-forward IS/OOS split fonctionne dans signal scanner
12. [ ] Grid search TP/SL injecte correctement les overrides
13. [ ] Daily PnL reset a UTC midnight
14. [ ] Emergency close se declenche au bon seuil
15. [ ] Les secrets sont wipes apres chargement
16. [ ] Aucune injection SQL possible
17. [ ] Le WebSocket se reconnecte automatiquement avec resubscription
18. [ ] `placed_at_idx` empeche les fills same-candle en backtest
19. [ ] `bt_time()` retourne des secondes (pas des millisecondes)
20. [ ] Les candles `get_indicators()` injectent `live_price` dans le close de la derniere bougie
