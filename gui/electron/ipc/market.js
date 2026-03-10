const path = require('path');
const fs = require('fs');

/* ── Cache ───────────────────────────────────────────────────────────────── */
let cached = null;
let fetchedAt = 0;
const CACHE_MS = 60_000; // 60s

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

  const data = {
    btc_price: 0,
    eth_price: 0,
    eth_btc: 0,
    btc_dominance: 0,
    total2_mcap: 0,
    gold: 0,
    sp500: 0,
    dxy: 0,
    fear_greed: 0,
    fear_greed_label: '',
    last_update: 0,
  };

  const apiKey = readMacroApiKey(projectRoot);

  // Fetch all sources in parallel
  const promises = [
    // 1. CoinGecko global (dominance, total mcap)
    fetchJSON('https://api.coingecko.com/api/v3/global'),
    // 2. CoinGecko prices (BTC, ETH, Gold via tether-gold)
    fetchJSON('https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,ethereum,tether-gold&vs_currencies=usd,btc'),
    // 3. Fear & Greed
    fetchJSON('https://api.alternative.me/fng/?limit=1'),
    // 4. S&P500 + DXY (only if API key available)
    apiKey
      ? fetchJSON(`https://financialmodelingprep.com/api/v3/quote/%5EGSPC,DX-Y.NYB?apikey=${apiKey}`)
      : Promise.resolve(null),
  ];

  const [globalData, pricesData, fngData, tradfiData] = await Promise.all(promises);

  // Parse CoinGecko global
  if (globalData?.data) {
    const d = globalData.data;
    data.btc_dominance = d.market_cap_percentage?.btc || 0;
    const totalMcap = d.total_market_cap?.usd || 0;
    if (totalMcap > 0 && data.btc_dominance > 0) {
      data.total2_mcap = (totalMcap * (1 - data.btc_dominance / 100)) / 1e9;
    }
  }

  // Parse CoinGecko prices
  if (pricesData) {
    data.btc_price = pricesData.bitcoin?.usd || 0;
    data.eth_price = pricesData.ethereum?.usd || 0;
    data.eth_btc = pricesData.ethereum?.btc || 0;
    data.gold = pricesData['tether-gold']?.usd || 0;
  }

  // Parse Fear & Greed
  if (fngData?.data?.[0]) {
    const fg = fngData.data[0];
    data.fear_greed = parseInt(fg.value, 10) || 0;
    data.fear_greed_label = fg.value_classification || '';
  }

  // Parse TradFi (S&P500, DXY)
  if (Array.isArray(tradfiData)) {
    for (const item of tradfiData) {
      if (item.symbol === '^GSPC') data.sp500 = item.price || 0;
      else if (item.symbol === 'DX-Y.NYB') data.dxy = item.price || 0;
    }
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
