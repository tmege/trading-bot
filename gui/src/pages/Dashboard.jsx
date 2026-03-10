import React from 'react';
import BotControls from '../components/BotControls';
import AccountPanel from '../components/AccountPanel';
import PositionTable from '../components/PositionTable';
import OrderTable from '../components/OrderTable';
import MarketPanel from '../components/MarketPanel';
import LogViewer from '../components/LogViewer';
import useLiveData from '../hooks/useLiveData';

export default function Dashboard({ botStatus, paperMode }) {
  const { positions, trades, account } = useLiveData();

  return (
    <div className="flex-1 flex flex-col overflow-hidden">
      {/* Paper trading banner */}
      {paperMode && (
        <div className="bg-yellow-500/15 border-b border-yellow-500/30 px-4 py-2 flex items-center justify-center gap-2 shrink-0">
          <span className="text-yellow-400 text-xs font-bold tracking-widest uppercase">
            Paper Trading
          </span>
          <span className="text-yellow-500/60 text-xs">
            — Simulated orders, no real funds
          </span>
        </div>
      )}

      {/* Top bar: controls + account */}
      <div className="p-4 pb-0 flex items-center justify-between">
        <BotControls botStatus={botStatus} />
      </div>

      {/* Content grid */}
      <div className="flex-1 p-4 overflow-auto space-y-4">
        <AccountPanel account={account} />

        <div className="grid grid-cols-2 gap-4">
          <PositionTable positions={positions} />
          <OrderTable trades={trades} />
        </div>

        <MarketPanel />

        <LogViewer />
      </div>
    </div>
  );
}
