import React from 'react';
import { DollarSign, TrendingUp, TrendingDown, Wallet } from 'lucide-react';

export default function AccountPanel({ account }) {
  const data = account || { dailyPnl: 0, dailyFees: 0, dailyTrades: 0, totalTrades: 0, balance: 0 };
  const pnlColor = data.dailyPnl >= 0 ? 'text-profit' : 'text-loss';
  const PnlIcon = data.dailyPnl >= 0 ? TrendingUp : TrendingDown;

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg p-4">
      <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Account</h3>
      <div className="grid grid-cols-4 gap-4">
        <div>
          <div className="flex items-center gap-1.5 mb-1">
            <Wallet size={14} className="text-accent" />
            <span className="text-xs text-gray-500">Balance</span>
          </div>
          <span className="text-lg font-bold text-white">
            {data.balance > 0 ? `$${data.balance.toFixed(2)}` : '$--'}
          </span>
        </div>
        <div>
          <div className="flex items-center gap-1.5 mb-1">
            <PnlIcon size={14} className={pnlColor} />
            <span className="text-xs text-gray-500">Daily P&L</span>
          </div>
          <span className={`text-lg font-bold ${pnlColor}`}>
            {data.dailyPnl >= 0 ? '+' : ''}{data.dailyPnl.toFixed(2)}$
          </span>
        </div>
        <div>
          <div className="flex items-center gap-1.5 mb-1">
            <DollarSign size={14} className="text-gray-500" />
            <span className="text-xs text-gray-500">Daily Fees</span>
          </div>
          <span className="text-lg font-bold text-gray-300">
            {data.dailyFees.toFixed(2)}$
          </span>
        </div>
        <div>
          <span className="text-xs text-gray-500 block mb-1">Trades Today</span>
          <span className="text-lg font-bold text-gray-300">
            {data.dailyTrades}
          </span>
        </div>
      </div>
    </div>
  );
}
