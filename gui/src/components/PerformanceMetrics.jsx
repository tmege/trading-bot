import React from 'react';
import { Activity, TrendingDown, DollarSign, Target, BarChart3, CalendarDays, AlertTriangle } from 'lucide-react';

// Marcus Venn alert thresholds (docs/marcus_venn_audit.md)
const THRESHOLDS = {
  weeklyPnlPct: -8,      // < -8% weekly = red alert
  dailyPnlPct: -4,       // < -4% daily = yellow alert
  winRate7d: 30,          // < 30% per strat = disable
  feeDragPct: 25,         // > 25% = warning
  tradesPerDay: 15,       // > 15 = over-trading
  maxDrawdownPct: 15,     // > 15% = stop all
};

function MetricCard({ icon: Icon, label, value, suffix = '', alert, alertMsg }) {
  const colorClass = alert === 'red'
    ? 'text-loss'
    : alert === 'yellow'
      ? 'text-yellow-400'
      : 'text-gray-300';

  return (
    <div className={`relative ${alert ? 'ring-1 ring-inset ' + (alert === 'red' ? 'ring-loss/40' : 'ring-yellow-500/40') : ''} rounded-lg`}>
      <div className="flex items-center gap-1.5 mb-1">
        <Icon size={14} className={alert ? colorClass : 'text-accent'} />
        <span className="text-xs text-gray-500">{label}</span>
        {alert && <AlertTriangle size={12} className={colorClass} />}
      </div>
      <span className={`text-lg font-bold ${colorClass}`}>
        {value}{suffix}
      </span>
      {alertMsg && (
        <div className={`text-[10px] mt-0.5 ${colorClass}`}>{alertMsg}</div>
      )}
    </div>
  );
}

export default function PerformanceMetrics({ metrics }) {
  if (!metrics) {
    return (
      <div className="bg-surface-card border border-surface-border rounded-lg p-4">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Performance</h3>
        <div className="text-xs text-gray-600">Insufficient data</div>
      </div>
    );
  }

  const {
    sharpe30d, maxDrawdownPct, feeDragPct, winRate,
    winTotal, lossTotal, tradesPerDay, weeklyPnl, weeklyPnlPct, dataPoints,
  } = metrics;

  // Alert logic
  const ddAlert = maxDrawdownPct > THRESHOLDS.maxDrawdownPct ? 'red'
    : maxDrawdownPct > THRESHOLDS.maxDrawdownPct * 0.7 ? 'yellow'
      : null;

  const feeAlert = feeDragPct > THRESHOLDS.feeDragPct ? 'red'
    : feeDragPct > THRESHOLDS.feeDragPct * 0.8 ? 'yellow'
      : null;

  const tpdAlert = tradesPerDay > THRESHOLDS.tradesPerDay ? 'red'
    : tradesPerDay > THRESHOLDS.tradesPerDay * 0.8 ? 'yellow'
      : null;

  const weekAlert = weeklyPnlPct < THRESHOLDS.weeklyPnlPct ? 'red'
    : weeklyPnlPct < THRESHOLDS.weeklyPnlPct * 0.5 ? 'yellow'
      : null;

  const sharpeAlert = sharpe30d !== null && sharpe30d < 0 ? 'yellow' : null;

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg p-4">
      <div className="flex items-center justify-between mb-3">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider">Performance — Marcus Venn</h3>
        {dataPoints > 0 && (
          <span className="text-[10px] text-gray-600">{dataPoints}j data</span>
        )}
      </div>
      <div className="grid grid-cols-2 sm:grid-cols-3 lg:grid-cols-6 gap-4">
        <MetricCard
          icon={Activity}
          label="Sharpe 30j"
          value={sharpe30d !== null ? sharpe30d.toFixed(2) : '--'}
          alert={sharpeAlert}
          alertMsg={sharpeAlert ? 'Sharpe negatif' : null}
        />
        <MetricCard
          icon={TrendingDown}
          label="Max Drawdown"
          value={maxDrawdownPct.toFixed(1)}
          suffix="%"
          alert={ddAlert}
          alertMsg={ddAlert === 'red' ? `>${THRESHOLDS.maxDrawdownPct}% — STOP` : ddAlert === 'yellow' ? 'Approche seuil' : null}
        />
        <MetricCard
          icon={DollarSign}
          label="Fee Drag"
          value={feeDragPct.toFixed(1)}
          suffix="%"
          alert={feeAlert}
          alertMsg={feeAlert ? `>${THRESHOLDS.feeDragPct}% du profit brut` : null}
        />
        <MetricCard
          icon={Target}
          label="Win Rate"
          value={winRate.toFixed(1)}
          suffix="%"
          alert={null}
          alertMsg={winTotal + lossTotal > 0 ? `${winTotal}W / ${lossTotal}L` : null}
        />
        <MetricCard
          icon={BarChart3}
          label="Trades/jour"
          value={tradesPerDay.toFixed(1)}
          alert={tpdAlert}
          alertMsg={tpdAlert ? 'Over-trading' : null}
        />
        <MetricCard
          icon={CalendarDays}
          label="P&L 7j"
          value={weeklyPnl >= 0 ? `+${weeklyPnl.toFixed(2)}` : weeklyPnl.toFixed(2)}
          suffix="$"
          alert={weekAlert}
          alertMsg={weekAlert === 'red' ? `${weeklyPnlPct.toFixed(1)}% — Review` : weekAlert === 'yellow' ? `${weeklyPnlPct.toFixed(1)}%` : weeklyPnlPct !== 0 ? `${weeklyPnlPct >= 0 ? '+' : ''}${weeklyPnlPct.toFixed(1)}%` : null}
        />
      </div>
    </div>
  );
}
