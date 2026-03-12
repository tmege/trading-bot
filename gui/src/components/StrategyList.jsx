import React from 'react';
import { FileCode, ToggleLeft, ToggleRight, Loader2, AlertTriangle } from 'lucide-react';

const ROLE_COLORS = {
  primary:   'bg-accent/20 text-accent',
  secondary: 'bg-purple-500/20 text-purple-400',
  inactive:  'bg-gray-700/30 text-gray-500',
};

// Marcus Venn: WR 7j < 30% per strat = disable warning
const WIN_RATE_ALERT = 30;

function aggregatePnl(file, stratPnl) {
  const base = file.replace(/\.lua$/, '');
  let totalPnl = 0;
  let totalTrades = 0;
  for (const [key, val] of Object.entries(stratPnl)) {
    if (key === base || key.startsWith(base + '_')) {
      totalPnl += val.totalPnl || 0;
      totalTrades += val.trades || 0;
    }
  }
  return { totalPnl, totalTrades };
}

function getStratDetail(file, stratDetails) {
  const base = file.replace(/\.lua$/, '');
  return stratDetails[base] || null;
}

export default function StrategyList({ strategies, selected, onSelect, onToggle, stratPnl = {}, stratDetails = {}, loading, togglingFile }) {
  if (loading) {
    return (
      <div className="flex items-center justify-center py-8 text-gray-500 text-sm gap-2">
        <Loader2 size={16} className="animate-spin" />
        Loading strategies...
      </div>
    );
  }

  return (
    <div className="space-y-2">
      {strategies.map((s) => {
        const pnl = aggregatePnl(s.file, stratPnl);
        const detail = getStratDetail(s.file, stratDetails);
        const isToggling = togglingFile === s.file;

        // Marcus Venn alert: win rate < 30% on 7d
        const hasWRAlert = detail && detail.winRate < WIN_RATE_ALERT && detail.totalTrades >= 5;
        // P&L 7d negative
        const has7dLoss = detail && detail.pnl7d < 0 && detail.trades7d >= 3;

        return (
          <div
            key={s.file}
            onClick={() => onSelect(s.file)}
            className={`p-3 rounded-lg border cursor-pointer transition-colors ${
              selected === s.file
                ? 'border-accent bg-accent/5'
                : hasWRAlert
                  ? 'border-loss/40 bg-loss/5 hover:bg-loss/10'
                  : 'border-surface-border bg-surface-card hover:bg-surface-hover'
            }`}
          >
            <div className="flex items-center justify-between mb-2">
              <div className="flex items-center gap-2">
                <FileCode size={14} className="text-gray-500" />
                <span className="text-sm font-medium text-white">{s.file}</span>
                {/* P&L badge */}
                {pnl.totalTrades > 0 && (
                  <span className={`text-[10px] font-mono font-medium ${
                    pnl.totalPnl >= 0 ? 'text-profit' : 'text-loss'
                  }`}>
                    {pnl.totalPnl >= 0 ? '+' : ''}{pnl.totalPnl.toFixed(2)}$ ({pnl.totalTrades})
                  </span>
                )}
                {hasWRAlert && (
                  <AlertTriangle size={12} className="text-loss" title="Win rate < 30%" />
                )}
              </div>
              <button
                onClick={(e) => {
                  e.stopPropagation();
                  if (!isToggling) onToggle(s.file, !s.active);
                }}
                className="text-gray-400 hover:text-white transition-colors"
              >
                {isToggling ? (
                  <Loader2 size={20} className="animate-spin text-accent" />
                ) : s.active ? (
                  <ToggleRight size={20} className="text-profit" />
                ) : (
                  <ToggleLeft size={20} className="text-gray-600" />
                )}
              </button>
            </div>

            {/* Row 2: role + status + coins */}
            <div className="flex items-center gap-2 flex-wrap">
              <span className={`px-2 py-0.5 rounded text-xs font-medium ${
                ROLE_COLORS[s.role] || ROLE_COLORS.inactive
              }`}>
                {s.role.toUpperCase()}
              </span>
              <span className={`text-xs ${s.active ? 'text-profit' : 'text-gray-600'}`}>
                {s.active ? 'Active' : 'Inactive'}
              </span>
              {detail && detail.totalTrades > 0 && (
                <span className="text-[10px] text-gray-500">
                  WR {detail.winRate.toFixed(0)}%
                </span>
              )}
              {detail && detail.trades7d > 0 && (
                <span className={`text-[10px] font-mono ${detail.pnl7d >= 0 ? 'text-profit' : 'text-loss'}`}>
                  7j: {detail.pnl7d >= 0 ? '+' : ''}{detail.pnl7d.toFixed(2)}$
                </span>
              )}
            </div>

            {/* Row 3: coin-level win rates */}
            {detail && Object.keys(detail.coins).length > 0 && (
              <div className="flex gap-1.5 mt-2 flex-wrap">
                {(s.coins || Object.keys(detail.coins)).map(c => {
                  const coinData = detail.coins[c];
                  if (!coinData || coinData.total === 0) {
                    return (
                      <span key={c} className="px-1.5 py-0.5 bg-accent/10 text-accent/80 rounded text-[10px] font-medium">
                        {c}
                      </span>
                    );
                  }
                  const wrColor = coinData.winRate >= 50 ? 'text-profit'
                    : coinData.winRate >= WIN_RATE_ALERT ? 'text-yellow-400'
                      : 'text-loss';
                  return (
                    <span key={c} className="px-1.5 py-0.5 bg-gray-800 rounded text-[10px] font-medium flex items-center gap-1">
                      <span className="text-accent/80">{c}</span>
                      <span className={wrColor}>{coinData.winRate.toFixed(0)}%</span>
                      <span className="text-gray-600">{coinData.total}t</span>
                    </span>
                  );
                })}
              </div>
            )}

            {/* No detail yet: show coin badges only */}
            {!detail && s.coins && s.coins.length > 0 && (
              <div className="flex gap-1 mt-2">
                {s.coins.map(c => (
                  <span key={c} className="px-1.5 py-0.5 bg-accent/10 text-accent/80 rounded text-[10px] font-medium">
                    {c}
                  </span>
                ))}
              </div>
            )}

            {/* Marcus Venn alert banner */}
            {hasWRAlert && (
              <div className="mt-2 px-2 py-1 bg-loss/10 border border-loss/20 rounded text-[10px] text-loss flex items-center gap-1">
                <AlertTriangle size={10} />
                Win rate {detail.winRate.toFixed(0)}% &lt; {WIN_RATE_ALERT}% — Marcus Venn: desactiver
              </div>
            )}
            {has7dLoss && !hasWRAlert && (
              <div className="mt-2 px-2 py-1 bg-yellow-500/10 border border-yellow-500/20 rounded text-[10px] text-yellow-400 flex items-center gap-1">
                <AlertTriangle size={10} />
                P&L 7j negatif: {detail.pnl7d.toFixed(2)}$
              </div>
            )}
          </div>
        );
      })}
    </div>
  );
}
