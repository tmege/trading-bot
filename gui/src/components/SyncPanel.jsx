import React, { useState, useEffect } from 'react';
import { RefreshCw, Database, Loader2, CheckCircle, AlertCircle } from 'lucide-react';

export default function SyncPanel() {
  const [status, setStatus] = useState(null);
  const [syncing, setSyncing] = useState(false);
  const [progress, setProgress] = useState(null);
  const [result, setResult] = useState(null);

  useEffect(() => {
    loadStatus();
    const id = setInterval(loadStatus, 30000);

    let cleanupProgress, cleanupAuto;
    if (window.api?.sync) {
      try {
        cleanupProgress = window.api.sync.onProgress((data) => {
          setProgress(data);
        });
        cleanupAuto = window.api.sync.onAutoComplete(() => {
          loadStatus();
          setResult({ auto: true });
        });
      } catch (_) {}
    }

    return () => {
      clearInterval(id);
      cleanupProgress?.();
      cleanupAuto?.();
    };
  }, []);

  async function loadStatus() {
    try {
      if (!window.api?.sync) return;
      const s = await window.api.sync.status();
      setStatus(s);
    } catch (_) {}
  }

  async function handleSync() {
    if (!window.api?.sync) {
      setResult({ ok: false, error: 'Sync API not available — restart the app' });
      return;
    }
    setSyncing(true);
    setResult(null);
    setProgress(null);
    try {
      const res = await window.api.sync.run();
      setResult(res);
      await loadStatus();
    } catch (err) {
      setResult({ ok: false, error: err.message });
    }
    setSyncing(false);
    setProgress(null);
  }

  const lastSync = status?.lastSync
    ? new Date(status.lastSync).toLocaleDateString('fr-FR', {
        day: 'numeric', month: 'short', year: 'numeric',
        hour: '2-digit', minute: '2-digit',
      })
    : 'Jamais';

  // Parse progress line
  let progressText = null;
  if (progress) {
    try {
      const p = typeof progress === 'string' ? JSON.parse(progress) : progress;
      if (p.coin && p.interval) {
        progressText = `${p.coin} / ${p.interval}`;
        if (p.fetched) progressText += ` (+${p.fetched})`;
      } else if (typeof progress === 'string') {
        progressText = progress.slice(0, 60);
      }
    } catch (_) {
      progressText = String(progress).slice(0, 60);
    }
  }

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg p-4">
      <div className="flex items-center justify-between mb-3">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider flex items-center gap-2">
          <Database size={12} />
          Market Data Cache
        </h3>
        <button
          onClick={handleSync}
          disabled={syncing}
          className="flex items-center gap-1.5 px-3 py-1.5 bg-accent/20 text-accent border border-accent/30 rounded text-xs font-medium hover:bg-accent/30 transition-colors disabled:opacity-50"
        >
          {syncing ? (
            <Loader2 size={12} className="animate-spin" />
          ) : (
            <RefreshCw size={12} />
          )}
          {syncing ? 'Syncing...' : 'Sync Data'}
        </button>
      </div>

      <div className="grid grid-cols-3 gap-3 text-sm">
        <div>
          <span className="text-gray-500 text-xs block">Last Sync</span>
          <span className="text-gray-300">{lastSync}</span>
        </div>
        <div>
          <span className="text-gray-500 text-xs block">Cache Size</span>
          <span className="text-gray-300">{status?.dbSizeMB || '0'} MB</span>
        </div>
        <div>
          <span className="text-gray-500 text-xs block">Status</span>
          {status?.needsSync ? (
            <span className="text-yellow-400 flex items-center gap-1">
              <AlertCircle size={12} />
              Update needed
            </span>
          ) : (
            <span className="text-profit flex items-center gap-1">
              <CheckCircle size={12} />
              Up to date
            </span>
          )}
        </div>
      </div>

      {/* Progress */}
      {syncing && progressText && (
        <div className="mt-3 px-2 py-1.5 bg-surface-bg rounded text-xs text-gray-400 font-mono truncate">
          {progressText}
        </div>
      )}

      {/* Result */}
      {result && !syncing && (
        <div className={`mt-3 px-2 py-1.5 rounded text-xs ${
          result.ok !== false
            ? 'bg-profit/10 text-profit'
            : 'bg-loss/10 text-loss'
        }`}>
          {result.ok !== false
            ? `Sync complete${result.new_candles ? ` — ${result.new_candles} new candles` : ''}`
            : `Error: ${result.error}`
          }
          {result.auto && ' (auto)'}
        </div>
      )}

      <div className="mt-3 text-xs text-gray-600">
        ETH, BTC, SOL, DOGE, HYPE, PUMP — 5m/15m/1h/4h/1d — 4 ans
      </div>
    </div>
  );
}
