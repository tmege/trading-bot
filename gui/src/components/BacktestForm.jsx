import React, { useState, useEffect } from 'react';
import { Play, Loader2 } from 'lucide-react';

const DEFAULT_COINS = ['ETH', 'BTC', 'SOL', 'DOGE', 'HYPE', 'PUMP'];
const INTERVALS = ['5m', '15m', '1h', '4h', '1d'];

function toLocalDate(d) {
  return d.toISOString().slice(0, 10);
}

function daysBetween(a, b) {
  return Math.round((b.getTime() - a.getTime()) / 86400000);
}

export default function BacktestForm({ onRun, running, activeCoins }) {
  const ALL_COINS = activeCoins && activeCoins.length > 0 ? activeCoins : DEFAULT_COINS;
  const [strategies, setStrategies] = useState([]);
  const [strategy, setStrategy] = useState('');
  const [selectedCoins, setSelectedCoins] = useState(['ETH', 'BTC']);
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

  function toggleCoin(coin) {
    setSelectedCoins(prev => {
      if (prev.includes(coin)) {
        if (prev.length <= 1) return prev; // minimum 1 coin
        return prev.filter(c => c !== coin);
      }
      return [...prev, coin];
    });
  }

  // Compute nDays and endDaysAgo from dates
  const start = new Date(startDate);
  const end = new Date(endDate);
  const now = new Date();
  const nDays = Math.max(1, daysBetween(start, end));
  const endDaysAgo = Math.max(0, daysBetween(end, now));

  function handleStrategyChange(file) {
    setStrategy(file);
    setSelectedCoins(['ETH', 'BTC']);
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
      coins: selectedCoins,
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

      {/* Coin selector pills */}
      <div className="mb-4 flex items-center gap-2">
        <span className="text-xs text-gray-400">Coins:</span>
        <div className="flex gap-1.5">
          {ALL_COINS.map(c => {
            const active = selectedCoins.includes(c);
            return (
              <button
                key={c}
                type="button"
                onClick={() => toggleCoin(c)}
                className={`px-2.5 py-0.5 rounded text-xs font-medium transition-colors ${
                  active
                    ? 'bg-accent/20 text-accent border border-accent/30'
                    : 'bg-surface-bg text-gray-500 border border-surface-border cursor-pointer hover:border-accent/30'
                }`}
              >
                {c}
              </button>
            );
          })}
        </div>
      </div>

      <button
        type="submit"
        disabled={running || !strategy || nDays < 1}
        className="flex items-center gap-2 px-4 py-2 bg-accent/20 text-accent border border-accent/30 rounded-lg text-sm font-medium hover:bg-accent/30 transition-colors disabled:opacity-50"
      >
        {running ? <Loader2 size={14} className="animate-spin" /> : <Play size={14} />}
        {running ? 'Running...' : `Run Backtest (${selectedCoins.length} coin${selectedCoins.length > 1 ? 's' : ''})`}
      </button>
    </form>
  );
}
