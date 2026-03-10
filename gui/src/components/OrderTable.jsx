import React from 'react';

export default function OrderTable({ trades }) {
  if (!trades || trades.length === 0) {
    return (
      <div className="bg-surface-card border border-surface-border rounded-lg p-4">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Recent Trades</h3>
        <p className="text-sm text-gray-600">No recent trades</p>
      </div>
    );
  }

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg p-4">
      <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Recent Trades</h3>
      <div className="max-h-[200px] overflow-y-auto">
      <table className="w-full text-sm">
        <thead>
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
    </div>
  );
}
