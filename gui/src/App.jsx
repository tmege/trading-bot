import React, { useState, useEffect, useCallback } from 'react';
import Sidebar from './components/Sidebar';
import Dashboard from './pages/Dashboard';
import Strategies from './pages/Strategies';
import Backtest from './pages/Backtest';
import Settings from './pages/Settings';
import TradeNotifications from './components/TradeNotifications';
import useBotStatus from './hooks/useBotStatus';
import useTradeNotifications from './hooks/useTradeNotifications';

const PAGES = {
  dashboard: Dashboard,
  strategies: Strategies,
  backtest: Backtest,
  settings: Settings,
};

export default function App() {
  const [page, setPage] = useState('dashboard');
  const botStatus = useBotStatus();
  const { notifications, dismiss } = useTradeNotifications();
  const [paperMode, setPaperMode] = useState(null);

  const loadPaperMode = useCallback(async () => {
    try {
      const res = await window.api.config.read();
      if (res.ok) setPaperMode(!!res.config.mode?.paper_trading);
    } catch (_) {}
  }, []);

  useEffect(() => { loadPaperMode(); }, [loadPaperMode]);

  const PageComponent = PAGES[page];

  return (
    <div className="flex flex-col h-screen w-screen bg-surface-bg">
      {/* Draggable titlebar */}
      <div className="titlebar h-9 shrink-0 flex items-center border-b border-surface-border bg-surface-card">
        {/* Traffic lights space (macOS) */}
        <div className="w-20 shrink-0" />
        <span className="text-xs text-gray-500 font-medium tracking-wider select-none">
          TRADING BOT
        </span>
      </div>
      <TradeNotifications notifications={notifications} onDismiss={dismiss} />
      <div className="flex flex-1 min-h-0">
        <Sidebar currentPage={page} onNavigate={setPage} botStatus={botStatus} />
        <main className="flex-1 flex flex-col overflow-hidden">
          <PageComponent botStatus={botStatus} paperMode={paperMode} onPaperModeChange={setPaperMode} />
        {/* Status bar */}
        <div className="h-7 flex items-center px-4 gap-4 border-t border-surface-border bg-surface-card text-xs text-gray-500 shrink-0">
          <div className="flex items-center gap-1.5">
            <span className={`w-2 h-2 rounded-full ${
              botStatus.running ? 'bg-profit animate-pulse' : 'bg-gray-600'
            }`} />
            <span>{botStatus.running ? 'Running' : 'Stopped'}</span>
          </div>
          {botStatus.running && (
            <span>Uptime: {formatUptime(botStatus.uptimeSec)}</span>
          )}
          {botStatus.running && botStatus.pid && (
            <span>PID: {botStatus.pid}</span>
          )}
        </div>
        </main>
      </div>
    </div>
  );
}

function formatUptime(sec) {
  if (!sec) return '0s';
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}
