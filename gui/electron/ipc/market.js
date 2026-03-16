/* ── Cache ───────────────────────────────────────────────────────────────── */
let cached = null;
let fetchedAt = 0;
const CACHE_MS = 120_000;       // 120s — crypto data (CoinGecko, F&G, forex)

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

async function fetchMarketData(projectRoot) {
  const now = Date.now();
  if (cached && now - fetchedAt < CACHE_MS) return cached;

  const data = cached ? { ...cached } : {
    btc_dominance: 0,
    total1_mcap: 0,
    total2_mcap: 0,
    total3_mcap: 0,
    fear_greed: 0,
    fear_greed_label: '',
    forex: [],
    last_update: 0,
  };

  // Fetch all sources in parallel
  const promises = [
    // 1. CoinGecko global (dominance, total mcap)
    fetchJSON('https://api.coingecko.com/api/v3/global'),
    // 2. Fear & Greed
    fetchJSON('https://api.alternative.me/fng/?limit=1'),
    // 3. Frankfurter: forex (no key needed)
    fetchJSON('https://api.frankfurter.app/latest?from=USD&to=EUR,GBP,JPY,CHF'),
  ];

  const settled = await Promise.allSettled(promises);
  const [globalData, fngData, forexData] = settled.map(
    (r) => r.status === 'fulfilled' ? r.value : null
  );

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
