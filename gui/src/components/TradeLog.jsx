import React, { useState, useMemo } from 'react';
import { ArrowUpDown, Download } from 'lucide-react';

const COLS = [
  { key: 'idx',           label: '#',       align: 'left' },
  { key: 'time_ms',       label: 'Time',    align: 'left' },
  { key: 'side',          label: 'Side',    align: 'left' },
  { key: 'price',         label: 'Price',   align: 'right' },
  { key: 'size',          label: 'Size',    align: 'right' },
  { key: 'pnl',           label: 'P&L',     align: 'right' },
  { key: 'fee',           label: 'Fee',     align: 'right' },
  { key: 'balance_after', label: 'Balance', align: 'right' },
];

const PAGE_SIZE = 50;

export default function TradeLog({ trades }) {
  const [sortKey, setSortKey] = useState(null);
  const [sortDir, setSortDir] = useState('asc');
  const [page, setPage] = useState(0);

  const indexed = useMemo(() =>
    (trades || []).map((t, i) => ({ ...t, idx: i + 1 })),
    [trades]
  );

  const sorted = useMemo(() => {
    if (!sortKey) return indexed;
    return [...indexed].sort((a, b) => {
      const va = a[sortKey] ?? 0;
      const vb = b[sortKey] ?? 0;
      return sortDir === 'asc' ? (va > vb ? 1 : -1) : (va < vb ? 1 : -1);
    });
  }, [indexed, sortKey, sortDir]);

  const totalPages = Math.ceil(sorted.length / PAGE_SIZE);
  const pageData = sorted.slice(page * PAGE_SIZE, (page + 1) * PAGE_SIZE);

  function handleSort(key) {
    if (sortKey === key) {
      setSortDir(d => d === 'asc' ? 'desc' : 'asc');
    } else {
      setSortKey(key);
      setSortDir('asc');
    }
  }

  function exportCsv() {
    const header = COLS.map(c => c.label).join(',');
    const rows = indexed.map(t =>
      [t.idx, new Date(t.time_ms).toISOString(), t.side, t.price?.toFixed(2),
       t.size?.toFixed(4), t.pnl?.toFixed(2), t.fee?.toFixed(4),
       t.balance_after?.toFixed(2)].join(',')
    );
    const csv = [header, ...rows].join('\n');
    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'backtest_trades.csv';
    a.click();
    URL.revokeObjectURL(url);
  }

  if (!trades || trades.length === 0) return null;

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg overflow-hidden">
      <div className="px-4 py-2 border-b border-surface-border flex items-center justify-between">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider">
          Trade Log ({trades.length} trades)
        </h3>
        <button
          onClick={exportCsv}
          className="flex items-center gap-1 text-xs text-gray-500 hover:text-gray-300 transition-colors"
        >
          <Download size={12} />
          CSV
        </button>
      </div>

      <div className="overflow-auto max-h-80">
        <table className="w-full text-xs">
          <thead className="sticky top-0 bg-surface-card">
            <tr>
              {COLS.map(col => (
                <th
                  key={col.key}
                  onClick={() => handleSort(col.key)}
                  className={`px-3 py-2 cursor-pointer hover:text-gray-200 text-gray-500 ${
                    col.align === 'right' ? 'text-right' : 'text-left'
                  }`}
                >
                  <span className="flex items-center gap-1">
                    {col.label}
                    {sortKey === col.key && (
                      <ArrowUpDown size={10} className="text-accent" />
                    )}
                  </span>
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {pageData.map((t) => (
              <tr key={t.idx} className="border-t border-surface-border hover:bg-surface-hover">
                <td className="px-3 py-1.5 text-gray-600">{t.idx}</td>
                <td className="px-3 py-1.5 text-gray-400">
                  {new Date(t.time_ms).toLocaleString()}
                </td>
                <td className={`px-3 py-1.5 font-medium ${
                  t.side === 'B' || t.side === 'buy' ? 'text-profit' : 'text-loss'
                }`}>
                  {t.side?.toUpperCase()}
                </td>
                <td className="px-3 py-1.5 text-right text-gray-300">
                  ${t.price?.toFixed(2)}
                </td>
                <td className="px-3 py-1.5 text-right text-gray-300">
                  {t.size?.toFixed(4)}
                </td>
                <td className={`px-3 py-1.5 text-right font-medium ${
                  t.pnl >= 0 ? 'text-profit' : 'text-loss'
                }`}>
                  {t.pnl !== 0 ? `${t.pnl >= 0 ? '+' : ''}${t.pnl?.toFixed(2)}$` : '-'}
                </td>
                <td className="px-3 py-1.5 text-right text-gray-600">
                  {t.fee?.toFixed(4)}$
                </td>
                <td className="px-3 py-1.5 text-right text-gray-300">
                  ${t.balance_after?.toFixed(2)}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {totalPages > 1 && (
        <div className="px-4 py-2 border-t border-surface-border flex items-center justify-between text-xs text-gray-500">
          <span>Page {page + 1} of {totalPages}</span>
          <div className="flex gap-2">
            <button
              onClick={() => setPage(p => Math.max(0, p - 1))}
              disabled={page === 0}
              className="px-2 py-1 border border-surface-border rounded hover:bg-surface-hover disabled:opacity-30"
            >
              Prev
            </button>
            <button
              onClick={() => setPage(p => Math.min(totalPages - 1, p + 1))}
              disabled={page >= totalPages - 1}
              className="px-2 py-1 border border-surface-border rounded hover:bg-surface-hover disabled:opacity-30"
            >
              Next
            </button>
          </div>
        </div>
      )}
    </div>
  );
}
