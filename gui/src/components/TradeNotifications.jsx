import React from 'react';
import { ArrowUpRight, ArrowDownRight, X } from 'lucide-react';

function formatPrice(price) {
  if (!price) return '--';
  if (price >= 1000) return price.toLocaleString('en-US', { maximumFractionDigits: 1 });
  if (price >= 1) return price.toFixed(3);
  return price.toFixed(5);
}

function formatSize(size) {
  if (!size) return '--';
  if (size >= 1) return size.toFixed(4);
  return size.toFixed(6);
}

function formatPnl(pnl) {
  const sign = pnl >= 0 ? '+' : '';
  return `${sign}${pnl.toFixed(4)}`;
}

export default function TradeNotifications({ notifications, onDismiss }) {
  if (notifications.length === 0) return null;

  return (
    <div className="fixed bottom-10 right-4 z-50 flex flex-col gap-2 pointer-events-none">
      {notifications.map((n) => (
        <div
          key={n.id}
          className={`pointer-events-auto animate-slide-in min-w-[280px] max-w-[340px] rounded-lg border shadow-lg backdrop-blur-sm ${
            n.isBuy
              ? 'bg-profit/10 border-profit/30'
              : 'bg-loss/10 border-loss/30'
          }`}
        >
          <div className="px-3 py-2.5">
            {/* Header: side + coin */}
            <div className="flex items-center justify-between mb-1.5">
              <div className="flex items-center gap-2">
                {n.isBuy ? (
                  <ArrowUpRight size={14} className="text-profit" />
                ) : (
                  <ArrowDownRight size={14} className="text-loss" />
                )}
                <span className={`text-xs font-bold tracking-wider ${n.isBuy ? 'text-profit' : 'text-loss'}`}>
                  {n.side}
                </span>
                <span className="text-sm font-semibold text-white">{n.coin}</span>
                {n.dir && (
                  <span className="text-[10px] text-gray-500">{n.dir}</span>
                )}
              </div>
              <button
                onClick={() => onDismiss(n.id)}
                className="text-gray-600 hover:text-white transition-colors"
              >
                <X size={12} />
              </button>
            </div>

            {/* Details */}
            <div className="flex items-center gap-4 text-xs">
              <div>
                <span className="text-gray-500">Size </span>
                <span className="text-white font-mono">{formatSize(n.size)}</span>
              </div>
              <div>
                <span className="text-gray-500">@ </span>
                <span className="text-white font-mono">${formatPrice(n.price)}</span>
              </div>
              <div>
                <span className="text-gray-500">Fee </span>
                <span className="text-gray-400 font-mono">${n.fee.toFixed(4)}</span>
              </div>
            </div>

            {/* PnL if close */}
            {n.isClose && (
              <div className={`mt-1.5 pt-1.5 border-t text-xs font-mono font-semibold ${
                n.closedPnl >= 0
                  ? 'border-profit/20 text-profit'
                  : 'border-loss/20 text-loss'
              }`}>
                P&L: {formatPnl(n.closedPnl)} USDC
              </div>
            )}
          </div>
        </div>
      ))}
    </div>
  );
}
