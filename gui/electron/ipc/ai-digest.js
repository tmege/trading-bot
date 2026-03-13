const fs = require('fs');
const path = require('path');
const env = require('../env');

/* ── RSS sources ────────────────────────────────────────────────────────── */
const RSS_FEEDS = [
  { name: 'CoinTelegraph', url: 'https://cointelegraph.com/rss' },
  { name: 'CoinDesk', url: 'https://www.coindesk.com/arc/outboundfeeds/rss/' },
  { name: 'Decrypt', url: 'https://decrypt.co/feed' },
  { name: 'TechCrunch Crypto', url: 'https://techcrunch.com/category/cryptocurrency/feed/' },
];

const MAX_ITEMS_PER_FEED = 5;   // extract top 5 articles per source
const ARTICLE_MAX_CHARS = 2000; // max chars per extracted article

/* Lazy-loaded article extractor (ESM module, loaded via dynamic import) */
let _extract = null;
async function getExtractor() {
  if (!_extract) {
    const mod = await import('@extractus/article-extractor');
    _extract = mod.extract;
  }
  return _extract;
}

/* ── State ──────────────────────────────────────────────────────────────── */
let memoryCache = null;

/** Cache is valid if generated_at is from today (same calendar day) */
function isSameDay(ts) {
  if (!ts) return false;
  const gen = new Date(ts);
  const now = new Date();
  return gen.getFullYear() === now.getFullYear()
    && gen.getMonth() === now.getMonth()
    && gen.getDate() === now.getDate();
}

/* ── Fetch with timeout ─────────────────────────────────────────────────── */
async function fetchText(url, timeoutMs = 10_000) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const res = await fetch(url, {
      signal: controller.signal,
      headers: {
        'User-Agent': 'Mozilla/5.0 (compatible; TradingBot/1.0)',
        'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
      },
    });
    if (!res.ok) return null;
    return await res.text();
  } catch (_) {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

/* ── Extract article content via article-extractor (trafilatura-like) ──── */
function stripHtml(str) {
  return (str || '')
    .replace(/<[^>]+>/g, ' ')
    .replace(/&amp;/g, '&')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#0?39;/g, "'")
    .replace(/&nbsp;/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
}

async function extractArticle(url) {
  try {
    const extract = await getExtractor();
    const article = await extract(url, {
      headers: {
        'User-Agent': 'Mozilla/5.0 (compatible; TradingBot/1.0)',
      },
    });
    if (!article) return null;
    // article.content is HTML — strip tags to get clean text
    const text = stripHtml(article.content || '');
    return text.slice(0, ARTICLE_MAX_CHARS) || null;
  } catch (_) {
    return null;
  }
}

/* ── Parse RSS XML (regex, no dependency) ───────────────────────────────── */
function parseRSS(xml) {
  const items = [];
  const itemRegex = /<item[\s>]([\s\S]*?)<\/item>/gi;
  let match;
  while ((match = itemRegex.exec(xml)) !== null && items.length < MAX_ITEMS_PER_FEED) {
    const block = match[1];
    const title = extractTag(block, 'title');
    const link = extractLink(block);
    const description = extractTag(block, 'description');
    if (title) {
      items.push({
        title,
        link,
        description: cleanHtml(description || ''),
      });
    }
  }
  return items;
}

function extractTag(xml, tag) {
  const cdataRe = new RegExp(`<${tag}[^>]*>\\s*<!\\[CDATA\\[([\\s\\S]*?)\\]\\]>\\s*</${tag}>`, 'i');
  const cdataMatch = xml.match(cdataRe);
  if (cdataMatch) return cdataMatch[1].trim();
  const re = new RegExp(`<${tag}[^>]*>([\\s\\S]*?)</${tag}>`, 'i');
  const m = xml.match(re);
  return m ? m[1].trim() : '';
}

function extractLink(block) {
  // <link>url</link> or <link><![CDATA[url]]></link>
  const link = extractTag(block, 'link');
  if (link) return link;
  // Sometimes link is just text between tags without wrapping
  const m = block.match(/<link[^>]*>\s*(https?:\/\/[^\s<]+)/i);
  return m ? m[1].trim() : '';
}

function cleanHtml(str) {
  return str
    .replace(/<[^>]+>/g, '')
    .replace(/&amp;/g, '&')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#0?39;/g, "'")
    .replace(/&nbsp;/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, 300);
}

/* ── Fetch all RSS feeds + scrape articles ──────────────────────────────── */
async function fetchAllFeeds() {
  // Phase 1: Fetch all RSS feeds in parallel
  const rssResults = await Promise.allSettled(
    RSS_FEEDS.map(async (feed) => {
      const xml = await fetchText(feed.url);
      if (!xml) return { name: feed.name, items: [] };
      return { name: feed.name, items: parseRSS(xml) };
    })
  );

  const feeds = rssResults
    .filter((r) => r.status === 'fulfilled')
    .map((r) => r.value);

  // Phase 2: Extract full article content in parallel (article-extractor)
  const extractJobs = [];
  for (const feed of feeds) {
    for (const item of feed.items) {
      if (item.link) {
        extractJobs.push(
          extractArticle(item.link).then((body) => {
            item.body = body || '';
          })
        );
      }
    }
  }
  await Promise.allSettled(extractJobs);

  return feeds;
}

/* ── Call Claude Haiku ──────────────────────────────────────────────────── */
async function generateDigest(feeds, apiKey) {
  const Anthropic = require('@anthropic-ai/sdk');
  const client = new Anthropic({ apiKey });

  // Build rich context from feeds with scraped article bodies
  let context = '';
  let articleCount = 0;
  for (const feed of feeds) {
    if (feed.items.length === 0) continue;
    context += `\n## ${feed.name}\n`;
    for (const item of feed.items) {
      articleCount++;
      context += `\n### ${item.title}\n`;
      if (item.body) {
        context += `${item.body}\n`;
      } else if (item.description) {
        context += `${item.description}\n`;
      }
    }
  }

  if (!context.trim()) {
    return {
      summary: 'Impossible de recuperer les actualites crypto. Reessayez plus tard.',
      sentiment: 'neutre',
      sources: RSS_FEEDS.map((f) => f.name),
    };
  }

  const response = await client.messages.create({
    model: 'claude-haiku-4-5-20251001',
    max_tokens: 4096,
    messages: [
      {
        role: 'user',
        content: `Voici ${articleCount} articles crypto recents de ${feeds.filter(f => f.items.length > 0).length} sources :\n${context}\n\nProduis un digest crypto du jour. Reponds UNIQUEMENT en JSON valide (pas de markdown, pas de texte avant/apres) :\n\n{"points":["point 1 (2-3 phrases max)","point 2",...],"sentiment":"bullish|bearish|neutre","sentiment_reason":"1 phrase","events":["event 1","event 2",...],"trends":["tendance 1","tendance 2"]}\n\nRegles :\n- 4 a 6 points, chacun 2-3 phrases MAX\n- sentiment + sentiment_reason obligatoires\n- 3-5 events impactants pour traders\n- 2-3 tendances emergentes\n- Tout en francais`,
      },
    ],
  });

  const text = response.content[0]?.text || '{}';
  const activeSources = feeds.filter((f) => f.items.length > 0).map((f) => f.name);

  function tryParseJSON(str) {
    const jsonMatch = str.match(/\{[\s\S]*\}/);
    if (!jsonMatch) return null;
    try {
      return JSON.parse(jsonMatch[0]);
    } catch (_) {
      // Try to repair truncated JSON: close open strings/arrays/objects
      let fixed = jsonMatch[0];
      // Close any unclosed string
      const quotes = (fixed.match(/"/g) || []).length;
      if (quotes % 2 !== 0) fixed += '"';
      // Close open arrays and objects
      const opens = (fixed.match(/\[/g) || []).length - (fixed.match(/\]/g) || []).length;
      const braces = (fixed.match(/\{/g) || []).length - (fixed.match(/\}/g) || []).length;
      for (let i = 0; i < opens; i++) fixed += ']';
      for (let i = 0; i < braces; i++) fixed += '}';
      try {
        return JSON.parse(fixed);
      } catch (_) {
        return null;
      }
    }
  }

  const parsed = tryParseJSON(text);
  if (parsed && parsed.points?.length > 0) {
    return {
      points: parsed.points,
      sentiment: parsed.sentiment || 'neutre',
      sentiment_reason: parsed.sentiment_reason || '',
      events: parsed.events || [],
      trends: parsed.trends || [],
      sources: activeSources,
      article_count: articleCount,
    };
  }

  // Last fallback: extract any readable text, never show raw JSON
  const cleaned = text
    .replace(/[{}\[\]"]/g, '')
    .replace(/points:|sentiment:|events:|trends:|sentiment_reason:/gi, '')
    .replace(/,\s*/g, '. ')
    .trim();
  return {
    points: cleaned ? [cleaned.slice(0, 500)] : ['Digest indisponible. Il sera regenere automatiquement demain.'],
    sentiment: 'neutre',
    sentiment_reason: '',
    events: [],
    trends: [],
    sources: activeSources,
    article_count: articleCount,
  };
}

/* ── Disk cache ─────────────────────────────────────────────────────────── */
function getDiskPath(projectRoot) {
  return path.join(projectRoot, 'data', 'ai_digest.json');
}

function loadFromDisk(projectRoot) {
  try {
    const raw = fs.readFileSync(getDiskPath(projectRoot), 'utf-8');
    return JSON.parse(raw);
  } catch (_) {
    return null;
  }
}

function saveToDisk(projectRoot, data) {
  try {
    const dir = path.join(projectRoot, 'data');
    if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(getDiskPath(projectRoot), JSON.stringify(data, null, 2));
  } catch (_) {
    // Non-fatal
  }
}

/* ── Main logic (1 call/day, before US market open 9:30 ET) ─────────────── */
async function getDigest(projectRoot) {
  const apiKey = env.getAnthropicApiKey();
  if (!apiKey) {
    const disk = loadFromDisk(projectRoot);
    if (disk) return { ok: true, data: disk, cached: true };
    return { ok: false, error: 'no_api_key' };
  }

  // Memory cache (same day)
  if (memoryCache && isSameDay(memoryCache.generated_at)) {
    return { ok: true, data: memoryCache, cached: true };
  }

  // Disk cache (same day)
  const disk = loadFromDisk(projectRoot);
  if (disk && isSameDay(disk.generated_at)) {
    memoryCache = disk;
    return { ok: true, data: disk, cached: true };
  }

  // Generate fresh digest: 1 call per day
  const now = Date.now();
  const feeds = await fetchAllFeeds();
  const digest = await generateDigest(feeds, apiKey);

  const result = {
    ...digest,
    generated_at: now,
  };

  memoryCache = result;
  saveToDisk(projectRoot, result);

  return { ok: true, data: result, cached: false };
}

/* ── Register IPC ───────────────────────────────────────────────────────── */
module.exports = function registerAiDigestIPC(ipcMain, projectRoot) {
  ipcMain.handle('ai-digest:get', async () => {
    try {
      return await getDigest(projectRoot);
    } catch (err) {
      return { ok: false, error: err.message };
    }
  });
};
