import React from 'react';
import { DollarSign, TrendingUp, TrendingDown, Wallet, ArrowUpDown } from 'lucide-react';

export default function AccountPanel({ account }) {
  const data = account || { dailyPnl: 0, unrealizedPnl: 0, dailyFees: 0, dailyTrades: 0, balance: 0 };
  const totalPnl = data.dailyPnl + (data.unrealizedPnl || 0);
  const totalColor = totalPnl >= 0 ? 'text-profit' : 'text-loss';
  const TotalIcon = totalPnl >= 0 ? TrendingUp : TrendingDown;
  const realizedColor = data.dailyPnl >= 0 ? 'text-profit' : 'text-loss';
  const unrealizedColor = (data.unrealizedPnl || 0) >= 0 ? 'text-profit' : 'text-loss';

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg p-4">
      <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Account</h3>
      <div className="grid grid-cols-5 gap-4">
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
            <TotalIcon size={14} className={totalColor} />
            <span className="text-xs text-gray-500">Daily P&L</span>
          </div>
          <span className={`text-lg font-bold ${totalColor}`}>
            {totalPnl >= 0 ? '+' : ''}{totalPnl.toFixed(2)}$
          </span>
          <div className="flex items-center gap-2 mt-0.5">
            <span className={`text-[10px] font-mono ${realizedColor}`}>
              R: {data.dailyPnl >= 0 ? '+' : ''}{data.dailyPnl.toFixed(2)}
            </span>
            <span className={`text-[10px] font-mono ${unrealizedColor}`}>
              U: {(data.unrealizedPnl || 0) >= 0 ? '+' : ''}{(data.unrealizedPnl || 0).toFixed(2)}
            </span>
          </div>
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
        <div>
          <div className="flex items-center gap-1.5 mb-1">
            <ArrowUpDown size={14} className="text-gray-500" />
            <span className="text-xs text-gray-500">Positions</span>
          </div>
          <span className="text-lg font-bold text-gray-300">
            {data.nPositions ?? '--'}
          </span>
        </div>
      </div>
    </div>
  );
}
