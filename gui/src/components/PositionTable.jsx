import React from 'react';

export default function PositionTable({ positions }) {
  if (!positions || positions.length === 0) {
    return (
      <div className="bg-surface-card border border-surface-border rounded-lg p-4">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Open Positions</h3>
        <p className="text-sm text-gray-600">No open positions</p>
      </div>
    );
  }

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg p-4">
      <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Open Positions</h3>
      <table className="w-full text-sm">
        <thead>
          <tr className="text-gray-500 text-xs">
            <th className="text-left pb-2">Coin</th>
            <th className="text-right pb-2">Size</th>
            <th className="text-right pb-2">Entry</th>
            <th className="text-right pb-2">uPnL</th>
            <th className="text-right pb-2">Lev</th>
          </tr>
        </thead>
        <tbody>
          {positions.map((p) => {
            const pnl = parseFloat(p.unrealized_pnl || '0');
            const pnlColor = pnl >= 0 ? 'text-profit' : 'text-loss';
            const sizeNum = parseFloat(p.size || '0');
            const side = sizeNum >= 0 ? 'LONG' : 'SHORT';
            const sideColor = sizeNum >= 0 ? 'text-profit' : 'text-loss';

            return (
              <tr key={p.coin} className="border-t border-surface-border">
                <td className="py-2">
                  <span className="font-medium text-white">{p.coin}</span>
                  <span className={`ml-2 text-xs ${sideColor}`}>{side}</span>
                </td>
                <td className="text-right py-2 text-gray-300">
                  {Math.abs(sizeNum).toFixed(4)}
                </td>
                <td className="text-right py-2 text-gray-300">
                  ${parseFloat(p.entry_px).toFixed(2)}
                </td>
                <td className={`text-right py-2 font-medium ${pnlColor}`}>
                  {pnl >= 0 ? '+' : ''}{pnl.toFixed(2)}$
                </td>
                <td className="text-right py-2 text-gray-500">
                  {p.leverage || '-'}x
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
