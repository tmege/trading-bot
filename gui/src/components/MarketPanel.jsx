import React, { useState, useEffect, useRef } from 'react';
import { Globe, Landmark, BarChart3, Coins, DollarSign } from 'lucide-react';
import usePrices from '../hooks/usePrices';

/* ── Formatters ──────────────────────────────────────────────────────────── */
function formatPrice(price) {
  if (!price) return '--';
  if (price >= 1000) return price.toLocaleString('en-US', { maximumFractionDigits: 1 });
  if (price >= 1) return price.toFixed(2);
  return price.toFixed(5);
}

function formatLarge(val) {
  if (!val) return '--';
  if (val >= 1000) return `${(val / 1000).toFixed(1)}T`;
  return `${val.toFixed(0)}B`;
}

function pctClass(pct) {
  if (pct > 0) return 'text-profit';
  if (pct < 0) return 'text-loss';
  return 'text-gray-400';
}

function formatPct(pct) {
  if (pct === undefined || pct === null || pct === 0) return '';
  const sign = pct > 0 ? '+' : '';
  return `${sign}${pct.toFixed(2)}%`;
}

function fgStyle(value) {
  if (value <= 25) return { color: 'text-loss', bg: 'bg-loss/15', border: 'border-loss/30' };
  if (value <= 45) return { color: 'text-orange-400', bg: 'bg-orange-400/15', border: 'border-orange-400/30' };
  if (value <= 55) return { color: 'text-yellow-400', bg: 'bg-yellow-400/15', border: 'border-yellow-400/30' };
  if (value <= 75) return { color: 'text-profit', bg: 'bg-profit/15', border: 'border-profit/30' };
  return { color: 'text-green-300', bg: 'bg-green-300/15', border: 'border-green-300/30' };
}

/* ── Reusable card ───────────────────────────────────────────────────────── */
function DataCard({ label, value, sub, valueClass = 'text-white', subClass }) {
  return (
    <div className="bg-surface-bg rounded-lg p-3">
      <div className="text-[10px] text-gray-500 uppercase mb-1">{label}</div>
      <div className={`text-sm font-bold font-mono ${valueClass}`}>{value}</div>
      {sub && <div className={`text-[10px] font-mono mt-0.5 ${subClass || 'text-gray-500'}`}>{sub}</div>}
    </div>
  );
}

function QuoteRow({ symbol, name, price, change_pct }) {
  return (
    <div className="flex items-center justify-between py-2 px-3 bg-surface-bg rounded-lg">
      <div className="flex items-center gap-2 min-w-0">
        <span className="text-sm font-semibold text-white truncate">{symbol}</span>
        {name && <span className="text-[10px] text-gray-500 truncate">{name}</span>}
      </div>
      <div className="flex items-center gap-3">
        <span className="text-sm font-mono text-gray-300">${formatPrice(price)}</span>
        {change_pct !== undefined && change_pct !== 0 && (
          <span className={`text-xs font-mono min-w-[52px] text-right ${pctClass(change_pct)}`}>
            {formatPct(change_pct)}
          </span>
        )}
      </div>
    </div>
  );
}

/* ── Section wrapper ─────────────────────────────────────────────────────── */
function Section({ icon: Icon, title, children, className = '' }) {
  return (
    <div className={`bg-surface-card border border-surface-border rounded-lg p-4 ${className}`}>
      <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3 flex items-center gap-2">
        <Icon size={12} />
        {title}
      </h3>
      {children}
    </div>
  );
}

/* ── Main component ──────────────────────────────────────────────────────── */
export default function MarketPanel({ marketData }) {
  const prices = usePrices();
  const macro = marketData;
  const prevPrices = useRef({});
  const [flashes, setFlashes] = useState({});

  useEffect(() => {
    const newFlashes = {};
    for (const coin of Object.keys(prices)) {
      const curr = prices[coin];
      const prev = prevPrices.current[coin];
      if (curr && prev && curr !== prev) {
        newFlashes[coin] = curr > prev ? 'up' : 'down';
      }
    }
    prevPrices.current = { ...prices };
    if (Object.keys(newFlashes).length > 0) {
      setFlashes(newFlashes);
      const t = setTimeout(() => setFlashes({}), 300);
      return () => clearTimeout(t);
    }
  }, [prices]);

  const fg = macro?.fear_greed ? fgStyle(macro.fear_greed) : null;

  return (
    <div className="space-y-4">
      {/* ── Crypto ──────────────────────────────────────────────────────── */}
      <Section icon={Globe} title={
        <span className="flex items-center gap-2">
          Crypto
          <span className="ml-auto flex items-center gap-1">
            <span className={`w-1.5 h-1.5 rounded-full ${Object.keys(prices).length > 0 ? 'bg-profit animate-pulse' : 'bg-gray-600'}`} />
            <span className="text-[10px] text-gray-600">{Object.keys(prices).length > 0 ? 'LIVE' : 'offline'}</span>
          </span>
        </span>
      }>
        {/* Live prices (WebSocket) */}
        {Object.keys(prices).length > 0 && (
          <div className="grid grid-cols-2 lg:grid-cols-3 gap-2 mb-3">
            {Object.entries(prices).map(([coin, price]) => {
              const flash = flashes[coin];
              const flashBg = flash === 'up' ? 'bg-profit/10' : flash === 'down' ? 'bg-loss/10' : 'bg-surface-bg';
              return (
                <div key={coin} className={`flex items-center justify-between py-2 px-3 rounded-lg transition-colors duration-300 ${flashBg}`}>
                  <span className="text-sm font-medium text-white">{coin}</span>
                  <span className={`text-sm font-mono ${flash === 'up' ? 'text-profit' : flash === 'down' ? 'text-loss' : 'text-gray-300'}`}>
                    ${formatPrice(price)}
                  </span>
                </div>
              );
            })}
          </div>
        )}

        {/* Market caps & dominance */}
        <div className="grid grid-cols-2 lg:grid-cols-4 gap-3 mb-3">
          {macro?.btc_dominance > 0 && (
            <DataCard label="BTC.D" value={`${macro.btc_dominance.toFixed(1)}%`} />
          )}
          {macro?.total1_mcap > 0 && (
            <DataCard label="TOTAL" value={`$${formatLarge(macro.total1_mcap)}`} sub="All crypto" />
          )}
          {macro?.total2_mcap > 0 && (
            <DataCard label="TOTAL2" value={`$${formatLarge(macro.total2_mcap)}`} sub="Excl. BTC" />
          )}
          {macro?.total3_mcap > 0 && (
            <DataCard label="TOTAL3" value={`$${formatLarge(macro.total3_mcap)}`} sub="Excl. BTC+ETH" />
          )}
        </div>

        {/* Fear & Greed */}
        {fg && macro?.fear_greed > 0 && (
          <div className={`${fg.bg} border ${fg.border} rounded-lg px-4 py-2.5 flex items-center gap-3`}>
            <span className={`text-lg font-bold font-mono ${fg.color}`}>{macro.fear_greed}</span>
            <div>
              <div className={`text-xs font-semibold ${fg.color}`}>{macro.fear_greed_label}</div>
              <div className="text-[10px] text-gray-500">Fear & Greed Index</div>
            </div>
            <div className="flex-1 ml-2">
              <div className="h-1.5 bg-surface-bg rounded-full overflow-hidden">
                <div
                  className={`h-full rounded-full transition-all ${fg.bg.replace('/15', '')}`}
                  style={{ width: `${macro.fear_greed}%` }}
                />
              </div>
            </div>
          </div>
        )}
      </Section>

      {/* ── Indices & ETFs ──────────────────────────────────────────────── */}
      {macro?.indices?.length > 0 && (
        <Section icon={Landmark} title="Indices">
          <div className="grid grid-cols-1 lg:grid-cols-2 gap-2">
            {macro.indices.map((idx) => (
              <QuoteRow key={idx.symbol} symbol={idx.symbol} price={idx.price} change_pct={idx.change_pct} />
            ))}
          </div>
        </Section>
      )}

      {/* ── Mega-Caps ───────────────────────────────────────────────────── */}
      {macro?.stocks?.length > 0 && (
        <Section icon={BarChart3} title="Mega-Caps">
          <div className="grid grid-cols-1 lg:grid-cols-2 gap-2">
            {macro.stocks.map((s) => (
              <QuoteRow key={s.symbol} symbol={s.symbol} name={s.name} price={s.price} change_pct={s.change_pct} />
            ))}
          </div>
        </Section>
      )}

      {/* ── Commodities ─────────────────────────────────────────────────── */}
      {(macro?.gold > 0 || macro?.silver > 0) && (
        <Section icon={Coins} title="Commodities">
          <div className="grid grid-cols-2 lg:grid-cols-4 gap-3">
            {macro.gold > 0 && (
              <DataCard
                label="Gold (XAU)"
                value={`$${formatPrice(macro.gold)}`}
                valueClass="text-yellow-400"
                sub={formatPct(macro.gold_pct)}
                subClass={pctClass(macro.gold_pct)}
              />
            )}
            {macro.silver > 0 && (
              <DataCard
                label="Silver (XAG)"
                value={`$${formatPrice(macro.silver)}`}
                valueClass="text-gray-300"
                sub={formatPct(macro.silver_pct)}
                subClass={pctClass(macro.silver_pct)}
              />
            )}
          </div>
        </Section>
      )}

      {/* ── Forex ───────────────────────────────────────────────────────── */}
      {macro?.forex?.length > 0 && (
        <Section icon={DollarSign} title="Forex">
          <div className="grid grid-cols-2 lg:grid-cols-4 gap-3">
            {macro.forex.map((fx) => (
              <DataCard key={fx.pair} label={fx.pair} value={fx.rate} />
            ))}
          </div>
        </Section>
      )}

      {/* ── Last update footer ──────────────────────────────────────────── */}
      {macro?.last_update > 0 && (
        <div className="text-[10px] text-gray-600 text-right">
          Updated {new Date(macro.last_update).toLocaleTimeString()}
          {!macro.has_fmp && ' — Stocks/commodities require TB_MACRO_API_KEY in .env'}
        </div>
      )}
    </div>
  );
}
