import React, { useState, useEffect, useCallback } from 'react';
import BotControls from '../components/BotControls';
import AccountPanel from '../components/AccountPanel';
import PerformanceMetrics from '../components/PerformanceMetrics';
import PositionTable from '../components/PositionTable';
import OrderTable from '../components/OrderTable';
import EquityCurve from '../components/EquityCurve';
import LogViewer from '../components/LogViewer';
import useLiveData from '../hooks/useLiveData';

export default function Dashboard({ botStatus, paperMode }) {
  const { positions, account } = useLiveData();
  const [equityCurve, setEquityCurve] = useState(null);
  const [perfMetrics, setPerfMetrics] = useState(null);

  const fetchEquity = useCallback(async () => {
    try {
      if (!window.api?.db?.equityCurve) return;
      const res = await window.api.db.equityCurve();
      if (res.ok && res.curve?.length > 0) setEquityCurve(res.curve);
    } catch (_) {}
  }, []);

  const fetchPerformance = useCallback(async () => {
    try {
      if (!window.api?.db?.performanceMetrics) return;
      const res = await window.api.db.performanceMetrics();
      if (res.ok) setPerfMetrics(res.metrics);
    } catch (_) {}
  }, []);

  useEffect(() => {
    fetchEquity();
    fetchPerformance();
    const eqId = setInterval(fetchEquity, 60000);
    const perfId = setInterval(fetchPerformance, 60000);
    return () => { clearInterval(eqId); clearInterval(perfId); };
  }, [fetchEquity, fetchPerformance]);

  return (
    <div className="flex-1 flex flex-col overflow-hidden">
      {/* Paper trading banner */}
      <div className="bg-yellow-500/15 border-b border-yellow-500/30 px-4 py-2 flex items-center justify-center gap-2 shrink-0">
        <span className="text-yellow-400 text-xs font-bold tracking-widest uppercase">
          Paper Trading — Educational
        </span>
        <span className="text-yellow-500/60 text-xs">
          — Simulated orders, no real funds
        </span>
      </div>

      {/* Top bar: controls + account */}
      <div className="p-4 pb-0 flex items-center justify-between">
        <BotControls botStatus={botStatus} />
      </div>

      {/* Content grid */}
      <div className="flex-1 p-4 overflow-auto space-y-4">
        <AccountPanel account={account} />
        <PerformanceMetrics metrics={perfMetrics} />

        {/* Equity curve */}
        {equityCurve && (
          <EquityCurve data={equityCurve} title="Portfolio Equity" />
        )}

        <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
          <PositionTable positions={positions} />
          <OrderTable />
        </div>

        <LogViewer />
      </div>
    </div>
  );
}
