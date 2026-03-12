const path = require('path');
const fs = require('fs');
let Database;
try {
  Database = require('better-sqlite3');
} catch (err) {
  console.error('[db] better-sqlite3 load failed:', err.message);
  Database = null;
}

let db = null;
let dbError = null;

function getDb(projectRoot) {
  if (db) return db;
  if (!Database) { dbError = 'better-sqlite3 not available'; return null; }

  const dbPath = path.join(projectRoot, 'data', 'trading_bot.db');
  try {
    db = new Database(dbPath, { readonly: true, fileMustExist: true });
    db.pragma('journal_mode = WAL');
    console.log('[db] opened:', dbPath);
    return db;
  } catch (err) {
    dbError = err.message;
    console.error('[db] open failed:', err.message);
    return null;
  }
}

// Read wallet address from .env
function readWalletAddress(projectRoot) {
  try {
    const envPath = path.join(projectRoot, '.env');
    const content = fs.readFileSync(envPath, 'utf-8');
    for (const line of content.split('\n')) {
      const trimmed = line.trim();
      if (trimmed.startsWith('TB_WALLET_ADDRESS=')) {
        let val = trimmed.slice('TB_WALLET_ADDRESS='.length).trim();
        if ((val.startsWith('"') && val.endsWith('"')) ||
            (val.startsWith("'") && val.endsWith("'"))) {
          val = val.slice(1, -1);
        }
        return val;
      }
    }
  } catch (_) {}
  return null;
}

// Fetch account data from Hyperliquid API
let cachedAccount = { balance: 0, dailyPnl: 0, dailyFees: 0, cumulativePnl: 0, positions: [] };
let accountFetchedAt = 0;
const ACCOUNT_CACHE_MS = 5000; // 5s cache

// Cumulative P&L from Hyperliquid (all-time fills, cached 60s)
let cachedCumulativePnl = 0;
let cumulativeFetchedAt = 0;
const CUMULATIVE_CACHE_MS = 60_000;

async function fetchHyperliquid(projectRoot) {
  const now = Date.now();
  if (now - accountFetchedAt < ACCOUNT_CACHE_MS) return cachedAccount;

  const addr = readWalletAddress(projectRoot);
  if (!addr) return cachedAccount;

  try {
    const todayMs = new Date().setHours(0, 0, 0, 0);

    // Fetch balance + daily fills + cumulative fills in parallel
    const needCumulative = (now - cumulativeFetchedAt >= CUMULATIVE_CACHE_MS);
    const [stateRes, fillsRes, cumRes] = await Promise.all([
      fetch('https://api.hyperliquid.xyz/info', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ type: 'clearinghouseState', user: addr }),
      }),
      fetch('https://api.hyperliquid.xyz/info', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          type: 'userFillsByTime',
          user: addr,
          startTime: todayMs,
        }),
      }),
      needCumulative
        ? fetch('https://api.hyperliquid.xyz/info', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ type: 'userFillsByTime', user: addr, startTime: 0 }),
          })
        : Promise.resolve(null),
    ]);

    if (stateRes.ok) {
      const state = await stateRes.json();
      const val = parseFloat(state?.marginSummary?.accountValue || '0');
      if (val > 0) cachedAccount.balance = val;

      // Extract open positions
      const positions = [];
      const assetPositions = state?.assetPositions || [];
      for (const ap of assetPositions) {
        const p = ap.position || ap;
        const size = parseFloat(p.szi || '0');
        if (size === 0) continue;
        positions.push({
          coin: p.coin,
          size: p.szi,
          entry_px: p.entryPx || '0',
          unrealized_pnl: p.unrealizedPnl || '0',
          leverage: p.leverage ? parseInt(p.leverage.value || p.leverage) : 0,
          liquidation_px: p.liquidationPx || null,
        });
      }
      cachedAccount.positions = positions;

      // Sum unrealized P&L across all open positions
      let unrealizedPnl = 0;
      for (const pos of positions) {
        unrealizedPnl += parseFloat(pos.unrealized_pnl || '0');
      }
      cachedAccount.unrealizedPnl = unrealizedPnl;
    }

    if (fillsRes.ok) {
      const fills = await fillsRes.json();
      if (Array.isArray(fills)) {
        let pnl = 0, fees = 0;
        for (const f of fills) {
          pnl += parseFloat(f.closedPnl || '0');
          fees += parseFloat(f.fee || '0');
        }
        cachedAccount.dailyPnl = pnl;
        cachedAccount.dailyFees = fees;
      }
    }

    // Cumulative P&L from all-time fills (cached 60s)
    if (cumRes && cumRes.ok) {
      const allFills = await cumRes.json();
      if (Array.isArray(allFills)) {
        let cumPnl = 0, cumFees = 0;
        for (const f of allFills) {
          cumPnl += parseFloat(f.closedPnl || '0');
          cumFees += Math.abs(parseFloat(f.fee || '0'));
        }
        cachedCumulativePnl = cumPnl - cumFees;
        cumulativeFetchedAt = now;
      }
    }
    cachedAccount.cumulativePnl = cachedCumulativePnl;

    accountFetchedAt = now;
  } catch (_) {}

  return cachedAccount;
}

// Candle cache DB (separate from trading_bot.db)
let cacheDb = null;
function getCacheDb(projectRoot) {
  if (cacheDb) return cacheDb;
  if (!Database) return null;
  const dbPath = path.join(projectRoot, 'data', 'candle_cache.db');
  try {
    cacheDb = new Database(dbPath, { readonly: true, fileMustExist: true });
    return cacheDb;
  } catch (_) {
    return null;
  }
}

module.exports = function registerDbIPC(ipcMain, projectRoot) {
  const emptyAccount = {
    dailyPnl: 0, dailyFees: 0, dailyTrades: 0, totalTrades: 0, balance: 0,
  };

  ipcMain.handle('db:positions', async () => {
    const hl = await fetchHyperliquid(projectRoot);
    return { ok: true, positions: hl.positions || [] };
  });

  ipcMain.handle('db:trades', async (_event, limit = 50) => {
    const conn = getDb(projectRoot);
    if (!conn) return { ok: true, trades: [], dbError };

    try {
      const safeLimit = Math.min(Math.max(1, Number(limit) || 50), 500);
      const rows = conn.prepare(
        `SELECT id, timestamp_ms, coin, side, price, size, fee, pnl, strategy
         FROM trades ORDER BY timestamp_ms DESC LIMIT ?`
      ).all(safeLimit);
      return { ok: true, trades: rows };
    } catch (err) {
      return { ok: true, trades: [], dbError: err.message };
    }
  });

  ipcMain.handle('db:dailyPnl', async () => {
    const conn = getDb(projectRoot);
    if (!conn) return { ok: true, dailyPnl: [] };

    try {
      const rows = conn.prepare(
        `SELECT date, realized_pnl, unrealized_pnl, fees_paid, n_trades
         FROM daily_pnl ORDER BY date DESC LIMIT 30`
      ).all();
      return { ok: true, dailyPnl: rows };
    } catch (_) {
      return { ok: true, dailyPnl: [] };
    }
  });

  // ── Filtered trades with pagination ────────────────────────────────────
  ipcMain.handle('db:filteredTrades', async (_event, params = {}) => {
    const conn = getDb(projectRoot);
    if (!conn) return { ok: true, trades: [], total: 0, dbError };

    try {
      const { coin, strategy, startMs, endMs, limit = 50, offset = 0 } = params;
      const safeLimit = Math.min(Math.max(1, Number(limit) || 50), 200);
      const safeOffset = Math.max(0, Number(offset) || 0);

      const conditions = [];
      const binds = [];

      if (coin && /^[A-Z]{2,10}$/.test(coin)) {
        conditions.push('coin = ?');
        binds.push(coin);
      }
      if (strategy && typeof strategy === 'string' && strategy.length < 100) {
        conditions.push('strategy = ?');
        binds.push(strategy);
      }
      if (startMs && Number.isFinite(Number(startMs))) {
        conditions.push('timestamp_ms >= ?');
        binds.push(Number(startMs));
      }
      if (endMs && Number.isFinite(Number(endMs))) {
        conditions.push('timestamp_ms <= ?');
        binds.push(Number(endMs));
      }

      const where = conditions.length > 0 ? `WHERE ${conditions.join(' AND ')}` : '';

      const countRow = conn.prepare(`SELECT COUNT(*) as total FROM trades ${where}`).get(...binds);
      const rows = conn.prepare(
        `SELECT id, timestamp_ms, coin, side, price, size, fee, pnl, strategy
         FROM trades ${where} ORDER BY timestamp_ms DESC LIMIT ? OFFSET ?`
      ).all(...binds, safeLimit, safeOffset);

      return { ok: true, trades: rows, total: countRow.total };
    } catch (err) {
      return { ok: true, trades: [], total: 0, dbError: err.message };
    }
  });

  // ── Trade filter options ──────────────────────────────────────────────
  ipcMain.handle('db:tradeFilters', async () => {
    const conn = getDb(projectRoot);
    if (!conn) return { ok: true, coins: [], strategies: [] };

    try {
      const coins = conn.prepare('SELECT DISTINCT coin FROM trades ORDER BY coin').all().map(r => r.coin);
      const strategies = conn.prepare('SELECT DISTINCT strategy FROM trades WHERE strategy IS NOT NULL ORDER BY strategy').all().map(r => r.strategy);
      return { ok: true, coins, strategies };
    } catch (_) {
      return { ok: true, coins: [], strategies: [] };
    }
  });

  // ── Equity curve (cumulative daily PnL) ───────────────────────────────
  ipcMain.handle('db:equityCurve', async () => {
    const conn = getDb(projectRoot);
    if (!conn) return { ok: true, curve: [] };

    try {
      const rows = conn.prepare(
        `SELECT date, realized_pnl, fees_paid
         FROM daily_pnl ORDER BY date ASC LIMIT 90`
      ).all();

      let equity = 100;
      const curve = rows.map(r => {
        equity += (parseFloat(r.realized_pnl) || 0) - (parseFloat(r.fees_paid) || 0);
        return {
          time_ms: new Date(r.date).getTime(),
          equity,
        };
      });

      return { ok: true, curve };
    } catch (_) {
      return { ok: true, curve: [] };
    }
  });

  // ── Strategy P&L aggregation ──────────────────────────────────────────
  ipcMain.handle('db:strategyPnl', async () => {
    const conn = getDb(projectRoot);
    if (!conn) return { ok: true, pnl: {} };

    try {
      const rows = conn.prepare(
        `SELECT strategy, SUM(CAST(pnl AS REAL)) as total_pnl, COUNT(*) as trades
         FROM trades WHERE strategy IS NOT NULL GROUP BY strategy`
      ).all();

      const pnl = {};
      for (const r of rows) {
        pnl[r.strategy] = { totalPnl: r.total_pnl || 0, trades: r.trades || 0 };
      }
      return { ok: true, pnl };
    } catch (_) {
      return { ok: true, pnl: {} };
    }
  });

  // ── Backtest-eligible coins (3+ years of data) ────────────────────────
  const MIN_YEARS = 3;
  ipcMain.handle('db:backtestCoins', async () => {
    const cc = getCacheDb(projectRoot);
    if (!cc) return { ok: true, coins: [] };

    try {
      const rows = cc.prepare(
        `SELECT coin, MIN(time_ms) as earliest, MAX(time_ms) as latest
         FROM candles
         GROUP BY coin`
      ).all();

      const minSpanMs = MIN_YEARS * 365.25 * 24 * 3600 * 1000;
      const coins = [];
      for (const r of rows) {
        const span = r.latest - r.earliest;
        if (span >= minSpanMs) {
          coins.push({
            coin: r.coin,
            years: +(span / (365.25 * 24 * 3600 * 1000)).toFixed(1),
          });
        }
      }
      coins.sort((a, b) => a.coin.localeCompare(b.coin));
      return { ok: true, coins };
    } catch (_) {
      return { ok: true, coins: [] };
    }
  });

  ipcMain.handle('db:account', async () => {
    const conn = getDb(projectRoot);
    const hl = await fetchHyperliquid(projectRoot);

    if (!conn) {
      return { ok: true, account: { ...emptyAccount, ...hl } };
    }

    try {
      const todayStartMs = new Date().setHours(0, 0, 0, 0);
      const daily = conn.prepare(
        'SELECT COUNT(*) as n_trades FROM trades WHERE timestamp_ms >= ?'
      ).get(todayStartMs);

      return {
        ok: true,
        account: {
          dailyPnl: hl.dailyPnl,
          unrealizedPnl: hl.unrealizedPnl || 0,
          dailyFees: hl.dailyFees,
          dailyTrades: daily.n_trades,
          nPositions: (hl.positions || []).length,
          balance: hl.balance,
          cumulativePnl: hl.cumulativePnl || 0,
        },
      };
    } catch (_) {
      return { ok: true, account: { ...emptyAccount, ...hl } };
    }
  });
};
