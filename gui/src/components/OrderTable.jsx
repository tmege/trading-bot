import React, { useState, useEffect, useCallback, useRef } from 'react';
import { Download } from 'lucide-react';

const FETCH_LIMIT = 100;

export default function OrderTable() {
  const [trades, setTrades] = useState([]);
  const [total, setTotal] = useState(0);
  const [filters, setFilters] = useState({ coins: [], strategies: [] });
  const [coinFilter, setCoinFilter] = useState('');
  const [stratFilter, setStratFilter] = useState('');
  const debounceRef = useRef(null);

  // Load filter options once
  useEffect(() => {
    async function loadFilters() {
      try {
        if (!window.api?.db?.tradeFilters) return;
        const res = await window.api.db.tradeFilters();
        if (res.ok) setFilters({ coins: res.coins || [], strategies: res.strategies || [] });
      } catch (_) {}
    }
    loadFilters();
  }, []);

  const fetchTrades = useCallback(async () => {
    try {
      if (!window.api?.db?.filteredTrades) {
        const res = await window.api.db.trades(FETCH_LIMIT);
        if (res.ok) { setTrades(res.trades); setTotal(res.trades.length); }
        return;
      }
      const params = { limit: FETCH_LIMIT, offset: 0 };
      if (coinFilter) params.coin = coinFilter;
      if (stratFilter) params.strategy = stratFilter;
      const res = await window.api.db.filteredTrades(params);
      if (res.ok) { setTrades(res.trades); setTotal(res.total); }
    } catch (_) {}
  }, [coinFilter, stratFilter]);

  // Debounced fetch
  useEffect(() => {
    clearTimeout(debounceRef.current);
    debounceRef.current = setTimeout(fetchTrades, 300);
    return () => clearTimeout(debounceRef.current);
  }, [fetchTrades]);

  // Auto-refresh every 10s
  useEffect(() => {
    const id = setInterval(fetchTrades, 10000);
    return () => clearInterval(id);
  }, [fetchTrades]);

  function exportCsv() {
    const header = 'Time,Coin,Side,Price,Size,Fee,P&L,Strategy';
    const rows = trades.map(t =>
      [new Date(t.timestamp_ms).toISOString(), t.coin, t.side, t.price, t.size, t.fee, t.pnl, t.strategy || ''].join(',')
    );
    const csv = [header, ...rows].join('\n');
    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `trades_${new Date().toISOString().slice(0, 10)}.csv`;
    a.click();
    URL.revokeObjectURL(url);
  }

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg p-4">
      {/* Header */}
      <div className="flex items-center justify-between mb-3">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider">
          Trades {total > 0 && `(${total})`}
        </h3>
        <button
          onClick={exportCsv}
          className="flex items-center gap-1 text-xs text-gray-500 hover:text-gray-300 transition-colors"
        >
          <Download size={12} />
          CSV
        </button>
      </div>

      {/* Filters */}
      <div className="flex gap-2 mb-3">
        <select
          value={coinFilter}
          onChange={e => setCoinFilter(e.target.value)}
          className="bg-surface-bg border border-surface-border rounded px-2 py-1 text-xs text-white outline-none focus:border-accent"
        >
          <option value="">All coins</option>
          {filters.coins.map(c => <option key={c} value={c}>{c}</option>)}
        </select>
        <select
          value={stratFilter}
          onChange={e => setStratFilter(e.target.value)}
          className="bg-surface-bg border border-surface-border rounded px-2 py-1 text-xs text-white outline-none focus:border-accent"
        >
          <option value="">All strategies</option>
          {filters.strategies.map(s => <option key={s} value={s}>{s}</option>)}
        </select>
      </div>

      {/* Table */}
      {trades.length === 0 ? (
        <p className="text-sm text-gray-600">No trades found</p>
      ) : (
          <div className="max-h-[290px] overflow-y-auto">
            <table className="w-full text-sm">
              <thead className="sticky top-0 bg-surface-card">
                <tr className="text-gray-500 text-xs">
                  <th className="text-left pb-2">Time</th>
                  <th className="text-left pb-2">Coin</th>
                  <th className="text-left pb-2">Side</th>
                  <th className="text-right pb-2">Price</th>
                  <th className="text-right pb-2">Size</th>
                  <th className="text-right pb-2">P&L</th>
                  <th className="text-left pb-2">Strategy</th>
                </tr>
              </thead>
              <tbody>
                {trades.map((t) => {
                  const pnl = parseFloat(t.pnl || '0');
                  const pnlColor = pnl >= 0 ? 'text-profit' : 'text-loss';
                  const sideColor = t.side === 'B' || t.side === 'buy' ? 'text-profit' : 'text-loss';
                  const time = new Date(t.timestamp_ms).toLocaleTimeString();

                  return (
                    <tr key={t.id} className="border-t border-surface-border">
                      <td className="py-1.5 text-gray-400 text-xs">{time}</td>
                      <td className="py-1.5 text-white">{t.coin}</td>
                      <td className={`py-1.5 text-xs font-medium ${sideColor}`}>
                        {t.side?.toUpperCase()}
                      </td>
                      <td className="py-1.5 text-right text-gray-300">
                        ${parseFloat(t.price).toFixed(2)}
                      </td>
                      <td className="py-1.5 text-right text-gray-300">
                        {parseFloat(t.size).toFixed(4)}
                      </td>
                      <td className={`py-1.5 text-right font-medium ${pnlColor}`}>
                        {pnl !== 0 ? `${pnl >= 0 ? '+' : ''}${pnl.toFixed(2)}$` : '-'}
                      </td>
                      <td className="py-1.5 text-gray-500 text-xs">{t.strategy || '-'}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
      )}
    </div>
  );
}
