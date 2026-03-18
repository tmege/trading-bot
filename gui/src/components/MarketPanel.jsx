import React, { useState, useEffect, useRef } from 'react';
import { Globe, DollarSign, Activity } from 'lucide-react';
import usePrices from '../hooks/usePrices';
import AiDigest from './AiDigest';

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

/* ── Market phase detection ─────────────────────────────────────────────── */
const PHASES = {
  BULL:     { label: 'Bull / Trend',      color: 'text-profit',     bg: 'bg-profit/15',      border: 'border-profit/30',      icon: '▲' },
  BEAR:     { label: 'Bear / Downtrend',  color: 'text-loss',       bg: 'bg-loss/15',         border: 'border-loss/30',        icon: '▼' },
  RANGE:    { label: 'Range / Sideways',  color: 'text-yellow-400', bg: 'bg-yellow-400/15',   border: 'border-yellow-400/30',  icon: '◆' },
  HIGH_VOL: { label: 'High Volatility',   color: 'text-orange-400', bg: 'bg-orange-400/15',   border: 'border-orange-400/30',  icon: '⚡' },
};

const PHASE_STRATEGIES = {
  BULL:     ['sniper_1h'],
  BEAR:     ['sniper_1h'],
  RANGE:    ['sniper_1h'],
  HIGH_VOL: ['sniper_1h'],
};

function detectMarketPhase(macro) {
  if (!macro) return null;
  const fg = macro.fear_greed || 50;

  if (fg <= 20 || fg >= 80) return 'HIGH_VOL';
  if (fg <= 40) return 'BEAR';
  if (fg >= 60) return 'BULL';
  return 'RANGE';
}

function MarketPhaseWidget({ macro }) {
  const phaseKey = detectMarketPhase(macro);
  if (!phaseKey) return null;
  const phase = PHASES[phaseKey];
  const strats = PHASE_STRATEGIES[phaseKey];

  return (
    <div className={`${phase.bg} border ${phase.border} rounded-lg px-4 py-3`}>
      <div className="flex items-center gap-2 mb-2">
        <Activity size={14} className={phase.color} />
        <span className={`text-xs font-semibold uppercase tracking-wider ${phase.color}`}>
          Market Phase
        </span>
      </div>
      <div className="flex items-center gap-2 mb-2">
        <span className={`text-lg font-bold ${phase.color}`}>{phase.icon}</span>
        <span className={`text-sm font-bold ${phase.color}`}>{phase.label}</span>
      </div>
      <div className="text-[10px] text-gray-500 mb-1.5">Best strategies for this phase:</div>
      <div className="flex flex-wrap gap-1.5">
        {strats.map((s) => (
          <span key={s} className="text-[10px] font-mono bg-surface-bg px-2 py-0.5 rounded text-gray-300">
            {s}
          </span>
        ))}
      </div>
    </div>
  );
}

/* ── Main component ──────────────────────────────────────────────────────── */
export default function MarketPanel({ marketData, activeCoins }) {
  const { prices, stale } = usePrices(activeCoins);
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
            <span className={`w-1.5 h-1.5 rounded-full ${Object.keys(prices).length > 0 && !stale ? 'bg-profit animate-pulse' : stale ? 'bg-yellow-400' : 'bg-gray-600'}`} />
            <span className="text-[10px] text-gray-600">
              {Object.keys(prices).length > 0 && !stale ? 'LIVE' : stale ? 'stale' : 'offline'}
            </span>
          </span>
        </span>
      }>
        {/* Live prices (WebSocket) */}
        {Object.keys(prices).length > 0 && (
          <div className={`grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-2 mb-3 ${stale ? 'opacity-50' : ''}`}>
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

      {/* ── Market Phase ──────────────────────────────────────────────────── */}
      <MarketPhaseWidget macro={macro} />

      {/* ── AI Digest ───────────────────────────────────────────────────── */}
      <AiDigest />

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
        </div>
      )}
    </div>
  );
}
