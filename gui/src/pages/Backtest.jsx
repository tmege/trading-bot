import React from 'react';
import BacktestForm from '../components/BacktestForm';
import StatsTable from '../components/StatsTable';
import EquityCurve from '../components/EquityCurve';
import TradeLog from '../components/TradeLog';
import CoinComparison from '../components/CoinComparison';
import useBacktest from '../hooks/useBacktest';
import { Loader2 } from 'lucide-react';

const VERDICT_COLORS = {
  DEPLOYABLE:   'text-profit border-profit/30 bg-profit/10',
  A_OPTIMISER:  'text-yellow-400 border-yellow-400/30 bg-yellow-400/10',
  MARGINAL:     'text-orange-400 border-orange-400/30 bg-orange-400/10',
  INSUFFISANT:  'text-gray-500 border-gray-600 bg-gray-500/10',
  ABANDON:      'text-loss border-loss/30 bg-loss/10',
};

export default function Backtest() {
  const { results, progress, running, error, run } = useBacktest();

  // Show first result details (or the one selected)
  const firstResult = results?.[0];

  return (
    <div className="flex-1 overflow-auto p-4 space-y-4">
      <BacktestForm onRun={run} running={running} />

      {/* Progress indicator */}
      {running && progress && (
        <div className="bg-surface-card border border-surface-border rounded-lg p-4 flex items-center gap-3">
          <Loader2 size={16} className="animate-spin text-accent" />
          <span className="text-sm text-gray-300">
            {progress.status === 'fetching' && `Fetching ${progress.coin} candles...`}
            {progress.status === 'running' && `Running backtest ${progress.detail || progress.coin}...`}
            {progress.status === 'starting' && 'Starting...'}
          </span>
          {progress.total > 1 && (
            <span className="text-xs text-gray-500 ml-auto">
              {progress.current}/{progress.total}
            </span>
          )}
        </div>
      )}

      {/* Error */}
      {error && (
        <div className="bg-loss/10 border border-loss/30 rounded-lg p-4 text-sm text-loss">
          {error}
        </div>
      )}

      {/* Results */}
      {firstResult && !firstResult.error && (
        <>
          {/* Verdict banner */}
          <div className={`border rounded-lg p-3 text-center font-bold text-lg ${
            VERDICT_COLORS[firstResult.verdict] || 'text-gray-500'
          }`}>
            {firstResult.verdict}
            <span className="text-sm font-normal ml-3 opacity-70">
              {firstResult.config?.coin} | {firstResult.config?.n_days}d | {firstResult.config?.interval}
            </span>
          </div>

          {/* Multi-coin comparison */}
          <CoinComparison results={results} />

          {/* Stats */}
          <StatsTable
            stats={firstResult.stats}
            statsIs={firstResult.stats_is}
            statsOos={firstResult.stats_oos}
            buyAndHold={firstResult.buy_and_hold}
            walkForward={firstResult.walk_forward}
          />

          {/* Equity Curve */}
          <EquityCurve data={firstResult.equity_curve} />

          {/* Trade Log */}
          <TradeLog trades={firstResult.trades} />
        </>
      )}

      {/* Error results from multi-coin */}
      {results?.filter(r => r.error).map(r => (
        <div key={r.coin} className="bg-loss/10 border border-loss/30 rounded-lg p-3 text-sm text-loss">
          {r.coin}: {r.error}
        </div>
      ))}
    </div>
  );
}
