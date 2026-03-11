import React, { useState, useEffect } from 'react';
import { Play, Loader2 } from 'lucide-react';

const ALL_COINS = ['ETH', 'BTC', 'SOL', 'DOGE', 'HYPE', 'PUMP'];
const INTERVALS = ['5m', '15m', '1h', '4h', '1d'];

function toLocalDate(d) {
  return d.toISOString().slice(0, 10);
}

function daysBetween(a, b) {
  return Math.round((b.getTime() - a.getTime()) / 86400000);
}

export default function BacktestForm({ onRun, running }) {
  const [strategies, setStrategies] = useState([]);
  const [strategy, setStrategy] = useState('');
  const [multiCoin, setMultiCoin] = useState(false);
  const [interval, setInterval] = useState('1h');

  // Date pickers — default: last 30 days
  const today = toLocalDate(new Date());
  const thirtyAgo = toLocalDate(new Date(Date.now() - 30 * 86400000));
  const [startDate, setStartDate] = useState(thirtyAgo);
  const [endDate, setEndDate] = useState(today);

  useEffect(() => {
    async function load() {
      try {
        const res = await window.api.strategies.list();
        if (res.ok) {
          setStrategies(res.strategies);
          if (res.strategies.length > 0 && !strategy) {
            const first = res.strategies[0].file;
            setStrategy(first);
            // Auto-detect interval from first strategy
            const base = first.replace(/\.lua$/, '');
            const lastSeg = base.split('_').pop();
            if (INTERVALS.includes(lastSeg)) {
              setInterval(lastSeg);
            }
          }
        }
      } catch (_) {}
    }
    load();
  }, []);

  const currentStrat = strategies.find(s => s.file === strategy);
  const stratCoins = currentStrat?.coins || [];
  const stratCoin = stratCoins[0] || null;
  const coins = multiCoin ? ALL_COINS : stratCoins.length > 0 ? stratCoins : ['ETH'];

  // Compute nDays and endDaysAgo from dates
  const start = new Date(startDate);
  const end = new Date(endDate);
  const now = new Date();
  const nDays = Math.max(1, daysBetween(start, end));
  const endDaysAgo = Math.max(0, daysBetween(end, now));

  function handleStrategyChange(file) {
    setStrategy(file);
    setMultiCoin(false);
    // Auto-detect interval from strategy filename (e.g. bb_scalp_15m.lua → 15m)
    const base = file.replace(/\.lua$/, '');
    const lastSeg = base.split('_').pop();
    if (INTERVALS.includes(lastSeg)) {
      setInterval(lastSeg);
    }
  }

  function handleSubmit(e) {
    e.preventDefault();
    onRun({
      strategy,
      coins,
      nDays,
      endDaysAgo,
      interval,
    });
  }

  return (
    <form onSubmit={handleSubmit} className="bg-surface-card border border-surface-border rounded-lg p-4">
      <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-4">Backtest Configuration</h3>

      <div className="grid grid-cols-3 gap-4 mb-4">
        <div>
          <label className="text-xs text-gray-400 block mb-1">Strategy</label>
          <select
            value={strategy}
            onChange={e => handleStrategyChange(e.target.value)}
            className="w-full bg-surface-bg border border-surface-border rounded px-3 py-2 text-sm text-white focus:border-accent outline-none"
          >
            {strategies.map(s => (
              <option key={s.file} value={s.file}>
                {s.file}{s.coin ? ` (${s.coin})` : ''}
              </option>
            ))}
          </select>
        </div>

        <div>
          <label className="text-xs text-gray-400 block mb-1">Interval (auto)</label>
          <select
            value={interval}
            disabled
            className="w-full bg-surface-bg border border-surface-border rounded px-3 py-2 text-sm text-white/60 outline-none cursor-not-allowed"
          >
            {INTERVALS.map(i => (
              <option key={i} value={i}>{i}</option>
            ))}
          </select>
        </div>

        <div className="flex items-end gap-2">
          <span className="text-xs text-gray-400 pb-2.5">{nDays}j</span>
        </div>

        <div>
          <label className="text-xs text-gray-400 block mb-1">Start</label>
          <input
            type="date"
            value={startDate}
            onChange={e => setStartDate(e.target.value)}
            max={endDate}
            className="w-full bg-surface-bg border border-surface-border rounded px-3 py-2 text-sm text-white focus:border-accent outline-none [color-scheme:dark]"
          />
        </div>

        <div>
          <label className="text-xs text-gray-400 block mb-1">End</label>
          <input
            type="date"
            value={endDate}
            onChange={e => setEndDate(e.target.value)}
            min={startDate}
            max={today}
            className="w-full bg-surface-bg border border-surface-border rounded px-3 py-2 text-sm text-white focus:border-accent outline-none [color-scheme:dark]"
          />
        </div>
      </div>

      {/* Coin display + multi-coin toggle */}
      <div className="mb-4 flex items-center gap-4">
        <div className="flex items-center gap-2">
          <span className="text-xs text-gray-400">Coins:</span>
          {stratCoins.length > 0 ? (
            <div className="flex gap-1">
              {stratCoins.map(c => (
                <span key={c} className="px-2 py-0.5 bg-accent/20 text-accent border border-accent/30 rounded text-xs font-medium">
                  {c}
                </span>
              ))}
            </div>
          ) : (
            <span className="text-xs text-gray-600">unknown</span>
          )}
        </div>

        <label className="flex items-center gap-2 cursor-pointer">
          <input
            type="checkbox"
            checked={multiCoin}
            onChange={e => setMultiCoin(e.target.checked)}
            className="rounded border-surface-border bg-surface-bg text-accent focus:ring-accent"
          />
          <span className="text-xs text-gray-400">All coins comparison</span>
        </label>

        {multiCoin && (
          <div className="flex gap-1.5">
            {ALL_COINS.map(c => (
              <span key={c} className="px-2 py-0.5 bg-accent/10 text-accent/80 rounded text-xs">
                {c}
              </span>
            ))}
          </div>
        )}
      </div>

      <button
        type="submit"
        disabled={running || !strategy || nDays < 1}
        className="flex items-center gap-2 px-4 py-2 bg-accent/20 text-accent border border-accent/30 rounded-lg text-sm font-medium hover:bg-accent/30 transition-colors disabled:opacity-50"
      >
        {running ? <Loader2 size={14} className="animate-spin" /> : <Play size={14} />}
        {running ? 'Running...' : `Run Backtest${multiCoin ? ' (5 coins)' : ''}`}
      </button>
    </form>
  );
}
