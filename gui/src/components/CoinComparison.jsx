import React from 'react';
import { Trophy } from 'lucide-react';

const VERDICT_COLORS = {
  DEPLOYABLE:   'text-profit bg-profit/10',
  A_OPTIMISER:  'text-yellow-400 bg-yellow-400/10',
  MARGINAL:     'text-orange-400 bg-orange-400/10',
  INSUFFISANT:  'text-gray-500 bg-gray-500/10',
  ABANDON:      'text-loss bg-loss/10',
};

export default function CoinComparison({ results }) {
  if (!results || results.length <= 1) return null;

  // Filter out errored results
  const valid = results.filter(r => !r.error && r.stats);

  if (valid.length <= 1) return null;

  // Find best Sharpe for highlighting
  const bestSharpe = Math.max(...valid.map(r => r.stats_oos?.sharpe_ratio || 0));

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg overflow-hidden">
      <div className="px-4 py-2 border-b border-surface-border flex items-center gap-2">
        <Trophy size={14} className="text-yellow-400" />
        <h3 className="text-xs text-gray-500 uppercase tracking-wider">
          Multi-Coin Comparison
        </h3>
      </div>

      <div className="overflow-auto">
        <table className="w-full text-sm">
          <thead>
            <tr className="text-gray-500 text-xs">
              <th className="text-left px-4 py-2">Coin</th>
              <th className="text-right px-3 py-2">Return</th>
              <th className="text-right px-3 py-2">Alpha</th>
              <th className="text-right px-3 py-2">Sharpe</th>
              <th className="text-right px-3 py-2">Max DD</th>
              <th className="text-right px-3 py-2">Trades</th>
              <th className="text-right px-3 py-2">Win Rate</th>
              <th className="text-right px-3 py-2">PF</th>
              <th className="text-center px-3 py-2">Verdict</th>
            </tr>
          </thead>
          <tbody>
            {valid.map((r) => {
              const s = r.stats_oos || r.stats;
              const sharpe = s?.sharpe_ratio || 0;
              const isBest = sharpe === bestSharpe && valid.length > 1;

              return (
                <tr
                  key={r.coin}
                  className={`border-t border-surface-border ${
                    isBest ? 'bg-profit/5' : ''
                  }`}
                >
                  <td className="px-4 py-2 font-medium text-white">
                    {r.coin}
                    {isBest && <span className="ml-2 text-xs text-profit">BEST</span>}
                  </td>
                  <td className={`px-3 py-2 text-right ${
                    s?.return_pct >= 0 ? 'text-profit' : 'text-loss'
                  }`}>
                    {s?.return_pct >= 0 ? '+' : ''}{s?.return_pct?.toFixed(1)}%
                  </td>
                  <td className={`px-3 py-2 text-right ${
                    r.alpha >= 0 ? 'text-profit' : 'text-loss'
                  }`}>
                    {r.alpha >= 0 ? '+' : ''}{r.alpha?.toFixed(1)}%
                  </td>
                  <td className="px-3 py-2 text-right text-gray-300">
                    {sharpe.toFixed(2)}
                  </td>
                  <td className="px-3 py-2 text-right text-gray-300">
                    {s?.max_drawdown_pct?.toFixed(1)}%
                  </td>
                  <td className="px-3 py-2 text-right text-gray-300">
                    {s?.total_trades}
                  </td>
                  <td className="px-3 py-2 text-right text-gray-300">
                    {s?.win_rate?.toFixed(1)}%
                  </td>
                  <td className="px-3 py-2 text-right text-gray-300">
                    {s?.profit_factor?.toFixed(2)}
                  </td>
                  <td className="px-3 py-2 text-center">
                    <span className={`px-2 py-0.5 rounded text-xs font-medium ${
                      VERDICT_COLORS[r.verdict] || 'text-gray-500'
                    }`}>
                      {r.verdict}
                    </span>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </div>
  );
}
