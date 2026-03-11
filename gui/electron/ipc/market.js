const path = require('path');
const fs = require('fs');

/* ── Cache ───────────────────────────────────────────────────────────────── */
let cached = null;
let fetchedAt = 0;
const CACHE_MS = 120_000;       // 120s — crypto data (CoinGecko, F&G, forex)

let fmpCached = null;
let fmpFetchedAt = 0;
const FMP_CACHE_MS = 3_600_000; // 1h — FMP free tier: 250 calls/day (16 syms × ~16 refreshes = ~256)

/* ── Read optional macro API key from .env ───────────────────────────────── */
function readMacroApiKey(projectRoot) {
  try {
    const envPath = path.join(projectRoot, '.env');
    const content = fs.readFileSync(envPath, 'utf-8');
    for (const line of content.split('\n')) {
      const trimmed = line.trim();
      if (trimmed.startsWith('TB_MACRO_API_KEY=')) {
        let val = trimmed.slice('TB_MACRO_API_KEY='.length).trim();
        if ((val.startsWith('"') && val.endsWith('"')) ||
            (val.startsWith("'") && val.endsWith("'"))) {
          val = val.slice(1, -1);
        }
        return val || null;
      }
    }
  } catch (_) {}
  return null;
}

/* ── FMP symbols (stable API, individual calls on free tier) ──────────── */
const FMP_INDICES = [
  { sym: '^GSPC',     label: 'S&P 500' },
  { sym: '^IXIC',     label: 'NASDAQ' },
  { sym: '^DJI',      label: 'Dow Jones' },
  { sym: '^HSI',      label: 'Hang Seng' },
  { sym: '^N225',     label: 'Nikkei 225' },
  { sym: '^STOXX50E', label: 'Euro Stoxx 50' },
  { sym: '^FTSE',     label: 'FTSE 100' },
];
const FMP_STOCKS  = ['AAPL', 'MSFT', 'NVDA', 'TSLA', 'GOOG', 'AMZN', 'META'];
const FMP_COMMODITIES = { GCUSD: 'gold', SIUSD: 'silver' };
const ALL_FMP_SYMBOLS = [
  ...FMP_INDICES.map(i => i.sym),
  ...FMP_STOCKS,
  ...Object.keys(FMP_COMMODITIES),
];

const STOCK_NAMES = {
  AAPL: 'Apple',   MSFT: 'Microsoft', NVDA: 'Nvidia',
  TSLA: 'Tesla',   GOOG: 'Alphabet',  AMZN: 'Amazon', META: 'Meta',
};

/* ── Fetch helpers ───────────────────────────────────────────────────────── */
async function fetchJSON(url, timeoutMs = 8000) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const res = await fetch(url, {
      signal: controller.signal,
      headers: { 'User-Agent': 'trading-bot-gui/1.0' },
    });
    if (!res.ok) return null;
    return await res.json();
  } catch (_) {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

/* ── Fetch all FMP symbols in parallel (individual calls, free tier) ───── */
async function fetchAllFmpQuotes(apiKey) {
  const results = await Promise.all(
    ALL_FMP_SYMBOLS.map(sym =>
      fetchJSON(`https://financialmodelingprep.com/stable/quote?symbol=${encodeURIComponent(sym)}&apikey=${apiKey}`)
    )
  );
  const map = {};
  for (const res of results) {
    if (Array.isArray(res) && res.length > 0 && res[0].symbol) {
      const item = res[0];
      map[item.symbol] = {
        price: item.price || 0,
        change_pct: item.changePercentage || 0,
        name: item.name || '',
      };
    }
  }
  return map;
}

async function fetchMarketData(projectRoot) {
  const now = Date.now();
  if (cached && now - fetchedAt < CACHE_MS) return cached;

  // Start from previous data to preserve FMP results across rate-limited refreshes
  const data = cached ? { ...cached } : {
    btc_dominance: 0,
    total1_mcap: 0,
    total2_mcap: 0,
    total3_mcap: 0,
    fear_greed: 0,
    fear_greed_label: '',
    indices: [],
    stocks: [],
    gold: 0,
    gold_pct: 0,
    silver: 0,
    silver_pct: 0,
    forex: [],
    last_update: 0,
    has_fmp: false,
  };

  const apiKey = readMacroApiKey(projectRoot);

  // FMP uses separate longer cache (250 calls/day free tier)
  const now2 = Date.now();
  let fmpQuotes = fmpCached;
  const needFmp = apiKey && (!fmpCached || now2 - fmpFetchedAt >= FMP_CACHE_MS);

  // Fetch all sources in parallel (1 CoinGecko call only to avoid rate limit)
  const promises = [
    // 1. CoinGecko global (dominance, total mcap)
    fetchJSON('https://api.coingecko.com/api/v3/global'),
    // 2. Fear & Greed
    fetchJSON('https://api.alternative.me/fng/?limit=1'),
    // 3. FMP: indices, stocks, commodities (separate cache, free tier)
    needFmp ? fetchAllFmpQuotes(apiKey) : Promise.resolve(null),
    // 4. Frankfurter: forex (always, no key needed)
    fetchJSON('https://api.frankfurter.app/latest?from=USD&to=EUR,GBP,JPY,CHF'),
  ];

  const settled = await Promise.allSettled(promises);
  const [globalData, fngData, freshFmpQuotes, forexData] = settled.map(
    (r) => r.status === 'fulfilled' ? r.value : null
  );

  if (freshFmpQuotes && Object.keys(freshFmpQuotes).length > 0) {
    fmpCached = freshFmpQuotes;
    fmpFetchedAt = now2;
    fmpQuotes = freshFmpQuotes;
  }

  // Parse CoinGecko global
  if (globalData?.data) {
    const d = globalData.data;
    data.btc_dominance = d.market_cap_percentage?.btc || 0;
    const ethDom = d.market_cap_percentage?.eth || 0;
    const totalMcap = d.total_market_cap?.usd || 0;
    if (totalMcap > 0) {
      data.total1_mcap = totalMcap / 1e9;
      data.total2_mcap = (totalMcap * (1 - data.btc_dominance / 100)) / 1e9;
      data.total3_mcap = (totalMcap * (1 - data.btc_dominance / 100 - ethDom / 100)) / 1e9;
    }
  }

  // Parse Fear & Greed
  if (fngData?.data?.[0]) {
    const fg = fngData.data[0];
    data.fear_greed = parseInt(fg.value, 10) || 0;
    data.fear_greed_label = fg.value_classification || '';
  }

  // Parse FMP quotes (indices, stocks, commodities) — only rebuild if fresh data
  if (fmpQuotes && Object.keys(fmpQuotes).length > 0) {
    data.has_fmp = true;
    const q = fmpQuotes;

    // Rebuild arrays from scratch to avoid duplicates
    data.indices = [];
    data.stocks = [];

    for (const idx of FMP_INDICES) {
      if (q[idx.sym]) data.indices.push({
        symbol: idx.label,
        price: q[idx.sym].price,
        change_pct: q[idx.sym].change_pct,
      });
    }

    for (const sym of FMP_STOCKS) {
      if (q[sym]) data.stocks.push({
        symbol: sym,
        name: STOCK_NAMES[sym] || q[sym].name,
        price: q[sym].price,
        change_pct: q[sym].change_pct,
      });
    }

    if (q.GCUSD) { data.gold = q.GCUSD.price; data.gold_pct = q.GCUSD.change_pct; }
    if (q.SIUSD) { data.silver = q.SIUSD.price; data.silver_pct = q.SIUSD.change_pct; }
  }
  // else: keep previous indices/stocks/commodities from cached data

  // Parse Frankfurter forex — rebuild to avoid duplicates
  if (forexData?.rates) {
    data.forex = [];
    const r = forexData.rates;
    if (r.EUR) data.forex.push({ pair: 'EUR/USD', rate: (1 / r.EUR).toFixed(4) });
    if (r.GBP) data.forex.push({ pair: 'GBP/USD', rate: (1 / r.GBP).toFixed(4) });
    if (r.JPY) data.forex.push({ pair: 'USD/JPY', rate: r.JPY.toFixed(2) });
    if (r.CHF) data.forex.push({ pair: 'USD/CHF', rate: r.CHF.toFixed(4) });
  }

  data.last_update = now;
  cached = data;
  fetchedAt = now;

  return data;
}

/* ── Register IPC ────────────────────────────────────────────────────────── */
module.exports = function registerMarketIPC(ipcMain, projectRoot) {
  ipcMain.handle('market:data', async () => {
    try {
      const data = await fetchMarketData(projectRoot);
      return { ok: true, data };
    } catch (err) {
      return { ok: false, error: err.message };
    }
  });
};
