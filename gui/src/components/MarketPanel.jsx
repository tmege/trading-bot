import React, { useState, useEffect, useRef } from 'react';
import { Activity, TrendingUp, TrendingDown, Globe, Gauge } from 'lucide-react';
import usePrices from '../hooks/usePrices';
import useMarketData from '../hooks/useMarketData';

function formatPrice(price) {
  if (!price) return '--';
  if (price >= 1000) return price.toLocaleString('en-US', { maximumFractionDigits: 1 });
  if (price >= 1) return price.toFixed(3);
  return price.toFixed(5);
}

function formatLarge(val) {
  if (!val) return '--';
  if (val >= 1000) return `${(val / 1000).toFixed(1)}T`;
  return `${val.toFixed(0)}B`;
}

/* Fear & Greed color + label */
function fgStyle(value) {
  if (value <= 25) return { color: 'text-loss', bg: 'bg-loss/15', border: 'border-loss/30', label: 'Extreme Fear' };
  if (value <= 45) return { color: 'text-orange-400', bg: 'bg-orange-400/15', border: 'border-orange-400/30', label: 'Fear' };
  if (value <= 55) return { color: 'text-yellow-400', bg: 'bg-yellow-400/15', border: 'border-yellow-400/30', label: 'Neutral' };
  if (value <= 75) return { color: 'text-profit', bg: 'bg-profit/15', border: 'border-profit/30', label: 'Greed' };
  return { color: 'text-green-300', bg: 'bg-green-300/15', border: 'border-green-300/30', label: 'Extreme Greed' };
}

export default function MarketPanel() {
  const prices = usePrices();
  const macro = useMarketData();
  const prevPrices = useRef({});
  const [flashes, setFlashes] = useState({});

  // Flash green/red on price change
  useEffect(() => {
    const newFlashes = {};
    const coins = Object.keys(prices);
    for (const coin of coins) {
      const curr = prices[coin];
      const prev = prevPrices.current[coin];
      if (curr && prev && curr !== prev) {
        newFlashes[coin] = curr > prev ? 'up' : 'down';
      }
    }
    prevPrices.current = { ...prices };
    if (Object.keys(newFlashes).length > 0) {
      setFlashes(newFlashes);
      const t = setTimeout(() => setFlashes({}), 300);
      return () => clearTimeout(t);
    }
  }, [prices]);

  const fg = macro?.fear_greed ? fgStyle(macro.fear_greed) : null;

  return (
    <div className="space-y-4">
      {/* Macro Overview */}
      {macro && (
        <div className="bg-surface-card border border-surface-border rounded-lg p-4">
          <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3 flex items-center gap-2">
            <Globe size={12} />
            Market Overview
          </h3>

          <div className="grid grid-cols-2 lg:grid-cols-4 gap-3">
            {/* BTC Price */}
            {macro.btc_price > 0 && (
              <div className="bg-surface-bg rounded-lg p-3">
                <div className="text-[10px] text-gray-500 uppercase mb-1">BTC</div>
                <div className="text-sm font-bold text-white font-mono">
                  ${formatPrice(macro.btc_price)}
                </div>
              </div>
            )}

            {/* BTC Dominance */}
            {macro.btc_dominance > 0 && (
              <div className="bg-surface-bg rounded-lg p-3">
                <div className="text-[10px] text-gray-500 uppercase mb-1">BTC Dom.</div>
                <div className="text-sm font-bold text-white font-mono">
                  {macro.btc_dominance.toFixed(1)}%
                </div>
              </div>
            )}

            {/* ETH/BTC */}
            {macro.eth_btc > 0 && (
              <div className="bg-surface-bg rounded-lg p-3">
                <div className="text-[10px] text-gray-500 uppercase mb-1">ETH/BTC</div>
                <div className="text-sm font-bold text-white font-mono">
                  {macro.eth_btc.toFixed(5)}
                </div>
              </div>
            )}

            {/* Total2 MCap */}
            {macro.total2_mcap > 0 && (
              <div className="bg-surface-bg rounded-lg p-3">
                <div className="text-[10px] text-gray-500 uppercase mb-1">Altcoin MCap</div>
                <div className="text-sm font-bold text-white font-mono">
                  ${formatLarge(macro.total2_mcap)}
                </div>
              </div>
            )}

            {/* Gold */}
            {macro.gold > 0 && (
              <div className="bg-surface-bg rounded-lg p-3">
                <div className="text-[10px] text-gray-500 uppercase mb-1">Gold</div>
                <div className="text-sm font-bold text-yellow-400 font-mono">
                  ${formatPrice(macro.gold)}
                </div>
              </div>
            )}

            {/* S&P 500 */}
            {macro.sp500 > 0 && (
              <div className="bg-surface-bg rounded-lg p-3">
                <div className="text-[10px] text-gray-500 uppercase mb-1">S&P 500</div>
                <div className="text-sm font-bold text-white font-mono">
                  {macro.sp500.toLocaleString('en-US', { maximumFractionDigits: 0 })}
                </div>
              </div>
            )}

            {/* DXY */}
            {macro.dxy > 0 && (
              <div className="bg-surface-bg rounded-lg p-3">
                <div className="text-[10px] text-gray-500 uppercase mb-1">DXY</div>
                <div className="text-sm font-bold text-white font-mono">
                  {macro.dxy.toFixed(1)}
                </div>
              </div>
            )}

            {/* Fear & Greed */}
            {fg && macro.fear_greed > 0 && (
              <div className={`${fg.bg} border ${fg.border} rounded-lg p-3`}>
                <div className="text-[10px] text-gray-500 uppercase mb-1">Fear & Greed</div>
                <div className="flex items-center gap-2">
                  <span className={`text-sm font-bold font-mono ${fg.color}`}>
                    {macro.fear_greed}
                  </span>
                  <span className={`text-[10px] ${fg.color}`}>
                    {macro.fear_greed_label}
                  </span>
                </div>
              </div>
            )}
          </div>
        </div>
      )}

      {/* Live Prices */}
      <div className="bg-surface-card border border-surface-border rounded-lg p-4">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3 flex items-center gap-2">
          <Activity size={12} />
          Live Prices
          <span className="ml-auto flex items-center gap-1">
            <span className={`w-1.5 h-1.5 rounded-full ${Object.keys(prices).length > 0 ? 'bg-profit animate-pulse' : 'bg-gray-600'}`} />
            <span className="text-[10px] text-gray-600">{Object.keys(prices).length > 0 ? 'WS' : 'offline'}</span>
          </span>
        </h3>
        <div className="grid grid-cols-2 lg:grid-cols-3 gap-2">
          {Object.entries(prices).map(([coin, price]) => {
            const flash = flashes[coin];
            const flashBg = flash === 'up' ? 'bg-profit/10' : flash === 'down' ? 'bg-loss/10' : 'bg-surface-bg';

            return (
              <div key={coin} className={`flex items-center justify-between py-2 px-3 rounded-lg transition-colors duration-300 ${flashBg}`}>
                <span className="text-sm font-medium text-white">{coin}</span>
                <span className={`text-sm font-mono ${flash === 'up' ? 'text-profit' : flash === 'down' ? 'text-loss' : 'text-gray-300'}`}>
                  ${formatPrice(price)}
                </span>
              </div>
            );
          })}
        </div>
      </div>
    </div>
  );
}
