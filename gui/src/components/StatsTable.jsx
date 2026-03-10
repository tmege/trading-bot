import React from 'react';

function StatRow({ label, value, color }) {
  return (
    <div className="flex justify-between py-1.5 border-b border-surface-border last:border-0">
      <span className="text-gray-400 text-sm">{label}</span>
      <span className={`text-sm font-medium ${color || 'text-gray-200'}`}>{value}</span>
    </div>
  );
}

function pnlColor(val) {
  if (val > 0) return 'text-profit';
  if (val < 0) return 'text-loss';
  return 'text-gray-400';
}

export default function StatsTable({ stats, statsIs, statsOos, buyAndHold, walkForward }) {
  if (!stats) return null;

  const bnh = buyAndHold?.full || {};

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg p-4">
      <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Statistics</h3>

      <div className="grid grid-cols-3 gap-4">
        {/* P&L Column */}
        <div>
          <h4 className="text-xs text-accent mb-2 font-medium">P&L (Full Period)</h4>
          <StatRow label="Net P&L" value={`${stats.net_pnl >= 0 ? '+' : ''}${stats.net_pnl?.toFixed(2)}$`} color={pnlColor(stats.net_pnl)} />
          <StatRow label="Return" value={`${stats.return_pct >= 0 ? '+' : ''}${stats.return_pct?.toFixed(1)}%`} color={pnlColor(stats.return_pct)} />
          <StatRow label="Trades" value={stats.total_trades} />
          <StatRow label="Win Rate" value={`${stats.win_rate?.toFixed(1)}%`} color={stats.win_rate >= 50 ? 'text-profit' : 'text-loss'} />
          <StatRow label="Profit Factor" value={stats.profit_factor?.toFixed(2)} color={stats.profit_factor >= 1.5 ? 'text-profit' : stats.profit_factor >= 1.0 ? 'text-yellow-400' : 'text-loss'} />
          <StatRow label="Avg Win" value={`+${stats.avg_win?.toFixed(2)}$`} color="text-profit" />
          <StatRow label="Avg Loss" value={`${stats.avg_loss?.toFixed(2)}$`} color="text-loss" />
          <StatRow label="Fees" value={`${stats.total_fees?.toFixed(2)}$`} color="text-gray-500" />
        </div>

        {/* Risk Column */}
        <div>
          <h4 className="text-xs text-accent mb-2 font-medium">Risk Metrics</h4>
          <StatRow label="Sharpe" value={stats.sharpe_ratio?.toFixed(2)} color={stats.sharpe_ratio >= 1.5 ? 'text-profit' : stats.sharpe_ratio >= 0.5 ? 'text-yellow-400' : 'text-loss'} />
          <StatRow label="Sortino" value={stats.sortino_ratio?.toFixed(2)} color={stats.sortino_ratio >= 1.5 ? 'text-profit' : 'text-gray-300'} />
          <StatRow label="Max DD" value={`${stats.max_drawdown_pct?.toFixed(1)}%`} color={stats.max_drawdown_pct < 20 ? 'text-profit' : 'text-loss'} />

          <h4 className="text-xs text-accent mb-2 mt-4 font-medium">Buy & Hold</h4>
          <StatRow label="B&H Return" value={`${bnh.return_pct >= 0 ? '+' : ''}${bnh.return_pct?.toFixed(1)}%`} color={pnlColor(bnh.return_pct)} />
          <StatRow label="B&H Sharpe" value={bnh.sharpe?.toFixed(2)} />
          <StatRow label="B&H Max DD" value={`${bnh.max_dd_pct?.toFixed(1)}%`} />
          <StatRow label="Alpha" value={`${(stats.return_pct - (bnh.return_pct || 0)) >= 0 ? '+' : ''}${(stats.return_pct - (bnh.return_pct || 0)).toFixed(1)}%`}
            color={pnlColor(stats.return_pct - (bnh.return_pct || 0))} />
        </div>

        {/* Walk-Forward Column */}
        <div>
          <h4 className="text-xs text-accent mb-2 font-medium">Walk-Forward IS/OOS</h4>
          {statsIs && statsOos && (
            <>
              <StatRow label="IS Return" value={`${statsIs.return_pct?.toFixed(1)}%`} color={pnlColor(statsIs.return_pct)} />
              <StatRow label="OOS Return" value={`${statsOos.return_pct?.toFixed(1)}%`} color={pnlColor(statsOos.return_pct)} />
              <StatRow label="IS Sharpe" value={statsIs.sharpe_ratio?.toFixed(2)} />
              <StatRow label="OOS Sharpe" value={statsOos.sharpe_ratio?.toFixed(2)} />
              <StatRow label="IS Trades" value={statsIs.total_trades} />
              <StatRow label="OOS Trades" value={statsOos.total_trades} />
            </>
          )}
          {walkForward && (
            <>
              <h4 className="text-xs text-accent mb-2 mt-4 font-medium">Decay</h4>
              <StatRow label="Sharpe Decay" value={`${walkForward.sharpe_decay_pct?.toFixed(1)}%`}
                color={walkForward.sharpe_decay_pct < -30 ? 'text-loss' : 'text-profit'} />
              <StatRow label="PF Decay" value={`${walkForward.pf_decay_pct?.toFixed(1)}%`}
                color={walkForward.pf_decay_pct < -30 ? 'text-loss' : 'text-profit'} />
              {walkForward.overfit_warning && (
                <div className="mt-2 px-2 py-1 bg-loss/10 border border-loss/20 rounded text-xs text-loss">
                  Overfit warning: OOS decay &gt; 50%
                </div>
              )}
            </>
          )}
        </div>
      </div>
    </div>
  );
}
