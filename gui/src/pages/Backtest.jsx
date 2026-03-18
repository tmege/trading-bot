import React, { useState, useEffect, useCallback } from 'react';
import BacktestForm from '../components/BacktestForm';
import StatsTable from '../components/StatsTable';
import EquityCurve from '../components/EquityCurve';
import TradeLog from '../components/TradeLog';
import CoinComparison from '../components/CoinComparison';
import ComparisonChart from '../components/ComparisonChart';
import useBacktest from '../hooks/useBacktest';
import { Loader2, Trophy, Trash2, GitCompare, X } from 'lucide-react';

const VERDICT_COLORS = {
  DEPLOYABLE:   'text-profit border-profit/30 bg-profit/10',
  A_OPTIMISER:  'text-yellow-400 border-yellow-400/30 bg-yellow-400/10',
  MARGINAL:     'text-orange-400 border-orange-400/30 bg-orange-400/10',
  INSUFFISANT:  'text-gray-500 border-gray-600 bg-gray-500/10',
  ABANDON:      'text-loss border-loss/30 bg-loss/10',
};

function formatReturn(pct) {
  if (pct >= 10000) return `${(pct / 1000).toFixed(0)}K%`;
  if (pct >= 1000) return `${(pct / 1000).toFixed(1)}K%`;
  return `${pct.toFixed(1)}%`;
}

export default function Backtest({ activeCoins }) {
  const { results, progress, running, error, run } = useBacktest();
  const [history, setHistory] = useState([]);
  const [comparisons, setComparisons] = useState([]);

  const loadHistory = useCallback(async () => {
    try {
      const h = await window.api.backtest.history();
      setHistory(h.sort((a, b) => b.return_pct - a.return_pct));
    } catch (_) {}
  }, []);

  useEffect(() => { loadHistory(); }, [loadHistory]);
  useEffect(() => { if (results && !running) loadHistory(); }, [results, running, loadHistory]);

  function addToCompare(result) {
    if (comparisons.length >= 3) return;
    const label = `${result.config?.coin || '?'} ${(result.config?.strategy || '').replace('.lua', '')}`;
    setComparisons(prev => [...prev, { label, equityCurve: result.equity_curve, stats: result.stats }]);
  }

  // Show first result details
  const firstResult = results?.[0];

  return (
    <div className="flex-1 overflow-auto p-4 space-y-4">
      <BacktestForm onRun={run} running={running} activeCoins={activeCoins} />

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
          {/* Verdict banner + compare button */}
          <div className={`border rounded-lg p-3 flex items-center justify-between ${
            VERDICT_COLORS[firstResult.verdict] || 'text-gray-500'
          }`}>
            <div>
              <span className="font-bold text-lg">{firstResult.verdict}</span>
              <span className="text-sm font-normal ml-3 opacity-70">
                {firstResult.config?.coin} | {firstResult.config?.n_days}d | {firstResult.config?.interval}
              </span>
            </div>
            {firstResult.equity_curve && comparisons.length < 3 && (
              <button
                onClick={() => addToCompare(firstResult)}
                className="flex items-center gap-1.5 px-3 py-1.5 text-xs bg-surface-bg border border-surface-border rounded-lg hover:border-accent/50 hover:text-accent transition-colors"
              >
                <GitCompare size={12} />
                Add to Compare ({comparisons.length}/3)
              </button>
            )}
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

      {/* ── Strategy Comparison ──────────────────────────────────────────── */}
      {comparisons.length > 0 && (
        <div className="space-y-3">
          <div className="flex items-center justify-between">
            <h3 className="text-xs text-gray-500 uppercase tracking-wider flex items-center gap-2">
              <GitCompare size={12} />
              Comparison ({comparisons.length}/3)
            </h3>
            <button
              onClick={() => setComparisons([])}
              className="flex items-center gap-1 text-[10px] text-gray-600 hover:text-loss"
            >
              <X size={10} /> Clear
            </button>
          </div>

          {comparisons.length >= 2 && (
            <>
              <ComparisonChart comparisons={comparisons} />

              {/* Comparison table */}
              <div className="bg-surface-card border border-surface-border rounded-lg overflow-hidden">
                <table className="w-full text-xs">
                  <thead>
                    <tr className="text-gray-500 text-left border-b border-surface-border">
                      <th className="px-3 py-2">Strategy</th>
                      <th className="px-3 py-2 text-right">Return</th>
                      <th className="px-3 py-2 text-right">Sharpe</th>
                      <th className="px-3 py-2 text-right">Max DD</th>
                      <th className="px-3 py-2 text-right">Win Rate</th>
                      <th className="px-3 py-2 text-right">Trades</th>
                    </tr>
                  </thead>
                  <tbody>
                    {comparisons.map((c, i) => {
                      const s = c.stats || {};
                      return (
                        <tr key={i} className="border-t border-surface-border/50">
                          <td className="px-3 py-2 text-gray-300">{c.label}</td>
                          <td className={`px-3 py-2 text-right font-mono ${(s.return_pct || 0) >= 0 ? 'text-profit' : 'text-loss'}`}>
                            {(s.return_pct || 0) >= 0 ? '+' : ''}{(s.return_pct || 0).toFixed(1)}%
                          </td>
                          <td className="px-3 py-2 text-right font-mono text-gray-300">
                            {(s.sharpe || 0).toFixed(2)}
                          </td>
                          <td className="px-3 py-2 text-right font-mono text-gray-400">
                            {(s.max_drawdown || 0).toFixed(1)}%
                          </td>
                          <td className="px-3 py-2 text-right font-mono text-gray-400">
                            {(s.win_rate || 0).toFixed(0)}%
                          </td>
                          <td className="px-3 py-2 text-right font-mono text-gray-400">
                            {s.total_trades || 0}
                          </td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            </>
          )}
        </div>
      )}

      {/* ── Backtest History Leaderboard ──────────────────────────────── */}
      {history.length > 0 && (
        <div className="bg-surface-card border border-surface-border rounded-lg p-4">
          <div className="flex items-center justify-between mb-3">
            <h3 className="text-xs text-gray-500 uppercase tracking-wider flex items-center gap-2">
              <Trophy size={12} />
              Backtest History ({history.length})
            </h3>
            <button
              className="text-xs text-gray-500 hover:text-loss flex items-center gap-1.5 px-2 py-1 rounded border border-transparent hover:border-loss/30 hover:bg-loss/10 transition-colors"
              onClick={async () => {
                if (!window.confirm(`Supprimer les ${history.length} entrées de l'historique ?`)) return;
                await window.api.backtest.clearHistory();
                setHistory([]);
              }}
            >
              <Trash2 size={12} /> Vider l'historique
            </button>
          </div>

          <div className="overflow-auto max-h-[400px]">
            <table className="w-full text-xs">
              <thead className="sticky top-0 bg-surface-card">
                <tr className="text-gray-500 text-left">
                  <th className="py-1.5 px-2">#</th>
                  <th className="py-1.5 px-2">Strategy</th>
                  <th className="py-1.5 px-2">Coin</th>
                  <th className="py-1.5 px-2">TF</th>
                  <th className="py-1.5 px-2 text-right">Return</th>
                  <th className="py-1.5 px-2 text-right">Sharpe</th>
                  <th className="py-1.5 px-2 text-right">Max DD</th>
                  <th className="py-1.5 px-2 text-right">WR</th>
                  <th className="py-1.5 px-2 text-right">Trades</th>
                  <th className="py-1.5 px-2 text-center">Verdict</th>
                  <th className="py-1.5 px-2 text-right">Days</th>
                </tr>
              </thead>
              <tbody>
                {history.map((h, i) => (
                  <tr key={h.id} className="border-t border-surface-border/50 hover:bg-surface-bg/50">
                    <td className="py-1.5 px-2 text-gray-600">{i + 1}</td>
                    <td className="py-1.5 px-2 font-mono text-gray-300">
                      {h.strategy.replace('.lua', '')}
                    </td>
                    <td className="py-1.5 px-2 text-white font-semibold">{h.coin}</td>
                    <td className="py-1.5 px-2 text-gray-400">{h.interval}</td>
                    <td className={`py-1.5 px-2 text-right font-mono font-bold ${
                      h.return_pct > 0 ? 'text-profit' : h.return_pct < 0 ? 'text-loss' : 'text-gray-400'
                    }`}>
                      {h.return_pct > 0 ? '+' : ''}{formatReturn(h.return_pct)}
                    </td>
                    <td className="py-1.5 px-2 text-right font-mono text-gray-300">
                      {h.sharpe?.toFixed(1)}
                    </td>
                    <td className="py-1.5 px-2 text-right font-mono text-gray-400">
                      {h.max_dd?.toFixed(2)}%
                    </td>
                    <td className="py-1.5 px-2 text-right font-mono text-gray-400">
                      {h.win_rate?.toFixed(0)}%
                    </td>
                    <td className="py-1.5 px-2 text-right font-mono text-gray-400">
                      {h.trades}
                    </td>
                    <td className="py-1.5 px-2 text-center">
                      <span className={`text-[10px] px-1.5 py-0.5 rounded border ${
                        VERDICT_COLORS[h.verdict] || 'text-gray-500'
                      }`}>
                        {h.verdict}
                      </span>
                    </td>
                    <td className="py-1.5 px-2 text-right text-gray-600">{h.n_days}d</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}
    </div>
  );
}
