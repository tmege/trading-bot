import React from 'react';
import { FileCode, ToggleLeft, ToggleRight, Loader2 } from 'lucide-react';

const ROLE_COLORS = {
  primary:   'bg-accent/20 text-accent',
  secondary: 'bg-purple-500/20 text-purple-400',
  inactive:  'bg-gray-700/30 text-gray-500',
};

function aggregatePnl(file, stratPnl) {
  // Match by prefix: bb_scalp_15m.lua → sum bb_scalp_15m_eth + bb_scalp_15m_btc + ...
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

export default function StrategyList({ strategies, selected, onSelect, onToggle, stratPnl = {}, loading, togglingFile }) {
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
        const isToggling = togglingFile === s.file;

        return (
          <div
            key={s.file}
            onClick={() => onSelect(s.file)}
            className={`p-3 rounded-lg border cursor-pointer transition-colors ${
              selected === s.file
                ? 'border-accent bg-accent/5'
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
            <div className="flex items-center gap-2 flex-wrap">
              <span className={`px-2 py-0.5 rounded text-xs font-medium ${
                ROLE_COLORS[s.role] || ROLE_COLORS.inactive
              }`}>
                {s.role.toUpperCase()}
              </span>
              <span className={`text-xs ${s.active ? 'text-profit' : 'text-gray-600'}`}>
                {s.active ? 'Active' : 'Inactive'}
              </span>
              {s.coins && s.coins.length > 0 && (
                <div className="flex gap-1">
                  {s.coins.map(c => (
                    <span key={c} className="px-1.5 py-0.5 bg-accent/10 text-accent/80 rounded text-[10px] font-medium">
                      {c}
                    </span>
                  ))}
                </div>
              )}
            </div>
          </div>
        );
      })}
    </div>
  );
}
