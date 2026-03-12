import React, { useState, useEffect, useCallback, useRef } from 'react';
import Sidebar from './components/Sidebar';
import Dashboard from './pages/Dashboard';
import Market from './pages/Market';
import Strategies from './pages/Strategies';
import Backtest from './pages/Backtest';
import Settings from './pages/Settings';
import TradeNotifications from './components/TradeNotifications';
import Toasts from './components/Toasts';
import LicenseGate from './pages/LicenseGate';
import useBotStatus from './hooks/useBotStatus';
import useTradeNotifications from './hooks/useTradeNotifications';
import useMarketData from './hooks/useMarketData';
import useToasts from './hooks/useToasts';
import useKeyboardShortcuts from './hooks/useKeyboardShortcuts';
import useActiveCoins from './hooks/useActiveCoins';

const PAGES = {
  dashboard: Dashboard,
  market: Market,
  strategies: Strategies,
  backtest: Backtest,
  settings: Settings,
};

export default function App() {
  const [licenseValid, setLicenseValid] = useState(null);

  useEffect(() => {
    window.api.license.check().then(res => {
      setLicenseValid(res.ok && res.valid);
    }).catch(() => setLicenseValid(false));
  }, []);

  if (licenseValid === null) {
    return (
      <div className="flex items-center justify-center h-screen w-screen bg-surface-bg">
        <div className="w-6 h-6 border-2 border-blue-500 border-t-transparent rounded-full animate-spin" />
      </div>
    );
  }

  if (!licenseValid) {
    return <LicenseGate onActivated={() => setLicenseValid(true)} />;
  }

  return <AppMain />;
}

function AppMain() {
  const [page, setPage] = useState('dashboard');
  const botStatus = useBotStatus();
  const { notifications, dismiss } = useTradeNotifications();
  const marketData = useMarketData();
  const { toasts, addToast, dismissToast } = useToasts();
  const [paperMode, setPaperMode] = useState(null);
  const [wsStatus, setWsStatus] = useState('connecting');
  const activeCoins = useActiveCoins();

  // Modal close callback ref (set by child pages)
  const closeModalRef = useRef(null);

  // Responsive sidebar collapse
  const [collapsed, setCollapsed] = useState(false);
  useEffect(() => {
    function check() { setCollapsed(window.innerWidth < 1100); }
    check();
    const ro = new ResizeObserver(check);
    ro.observe(document.body);
    return () => ro.disconnect();
  }, []);

  // Keyboard shortcuts
  useKeyboardShortcuts({
    onNavigate: setPage,
    onCloseModal: () => { if (closeModalRef.current) closeModalRef.current(); },
  });

  // WS status listener
  useEffect(() => {
    if (!window.api?.ws?.onStatus) return;
    return window.api.ws.onStatus((data) => {
      setWsStatus(data.status);
    });
  }, []);

  const loadPaperMode = useCallback(async () => {
    try {
      const res = await window.api.config.read();
      if (res.ok) setPaperMode(!!res.config.mode?.paper_trading);
    } catch (_) {}
  }, []);

  useEffect(() => { loadPaperMode(); }, [loadPaperMode]);

  const PageComponent = PAGES[page];

  const wsColor = wsStatus === 'connected' ? 'bg-profit' :
                  wsStatus === 'reconnecting' ? 'bg-yellow-400 animate-pulse' :
                  'bg-loss';
  const wsLabel = wsStatus === 'connected' ? 'WS Connected' :
                  wsStatus === 'reconnecting' ? 'WS Reconnecting...' :
                  'WS Disconnected';

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
      <Toasts toasts={toasts} onDismiss={dismissToast} />
      <div className="flex flex-1 min-h-0">
        <Sidebar currentPage={page} onNavigate={setPage} botStatus={botStatus} collapsed={collapsed} />
        <main className="flex-1 flex flex-col overflow-hidden">
          <PageComponent
            botStatus={botStatus}
            paperMode={paperMode}
            onPaperModeChange={setPaperMode}
            marketData={marketData}
            addToast={addToast}
            closeModalRef={closeModalRef}
            activeCoins={activeCoins}
          />
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
          {/* WS status indicator */}
          <div className="flex items-center gap-1.5 ml-auto">
            <span className={`w-2 h-2 rounded-full ${wsColor}`} />
            <span>{wsLabel}</span>
          </div>
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
