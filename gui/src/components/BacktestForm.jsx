import React, { useState, useEffect } from 'react';
import { Play, Loader2 } from 'lucide-react';

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
  const [availableCoins, setAvailableCoins] = useState([]);
  const [selectedCoins, setSelectedCoins] = useState([]);
  const [interval, setInterval] = useState('1h');

  // Date pickers — default: last 30 days
  const today = toLocalDate(new Date());
  const thirtyAgo = toLocalDate(new Date(Date.now() - 30 * 86400000));
  const [startDate, setStartDate] = useState(thirtyAgo);
  const [endDate, setEndDate] = useState(today);

  // Load strategies + available coins (3+ years of cache data)
  useEffect(() => {
    async function load() {
      try {
        const [stratRes, coinRes] = await Promise.all([
          window.api.strategies.list(),
          window.api.db.backtestCoins ? window.api.db.backtestCoins() : Promise.resolve({ ok: false }),
        ]);

        if (stratRes.ok) {
          setStrategies(stratRes.strategies);
          if (stratRes.strategies.length > 0 && !strategy) {
            const first = stratRes.strategies[0].file;
            setStrategy(first);
            const base = first.replace(/\.lua$/, '');
            const lastSeg = base.split('_').pop();
            if (INTERVALS.includes(lastSeg)) {
              setInterval(lastSeg);
            }
          }
        }

        if (coinRes.ok && coinRes.coins.length > 0) {
          const coins = coinRes.coins.map(c => c.coin);
          setAvailableCoins(coinRes.coins);
          // Pre-select first 2 coins
          setSelectedCoins(coins.slice(0, 2));
        }
      } catch (_) {}
    }
    load();
  }, []);

  function toggleCoin(coin) {
    setSelectedCoins(prev => {
      if (prev.includes(coin)) {
        if (prev.length <= 1) return prev;
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
    // Keep current selected coins (they're all valid since they have 3+ years)
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

      {/* Coin selector pills — only coins with 3+ years of data */}
      <div className="mb-4">
        <div className="flex items-center gap-2 mb-2">
          <span className="text-xs text-gray-400">Coins:</span>
          {availableCoins.length === 0 && (
            <span className="text-[10px] text-gray-600">No coins with 3+ years of cached data</span>
          )}
        </div>
        <div className="flex flex-wrap gap-1.5">
          {availableCoins.map(({ coin, years }) => {
            const active = selectedCoins.includes(coin);
            return (
              <button
                key={coin}
                type="button"
                onClick={() => toggleCoin(coin)}
                title={`${years} years of data`}
                className={`px-2.5 py-0.5 rounded text-xs font-medium transition-colors ${
                  active
                    ? 'bg-accent/20 text-accent border border-accent/30'
                    : 'bg-surface-bg text-gray-500 border border-surface-border cursor-pointer hover:border-accent/30'
                }`}
              >
                {coin}
                <span className="ml-1 text-[9px] opacity-60">{years}y</span>
              </button>
            );
          })}
        </div>
      </div>

      <button
        type="submit"
        disabled={running || !strategy || nDays < 1 || selectedCoins.length === 0}
        className="flex items-center gap-2 px-4 py-2 bg-accent/20 text-accent border border-accent/30 rounded-lg text-sm font-medium hover:bg-accent/30 transition-colors disabled:opacity-50"
      >
        {running ? <Loader2 size={14} className="animate-spin" /> : <Play size={14} />}
        {running ? 'Running...' : `Run Backtest (${selectedCoins.length} coin${selectedCoins.length > 1 ? 's' : ''})`}
      </button>
    </form>
  );
}
