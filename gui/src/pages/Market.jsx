import React from 'react';
import MarketPanel from '../components/MarketPanel';

export default function Market({ marketData, activeCoins }) {
  return (
    <div className="flex-1 overflow-auto p-6">
      <MarketPanel marketData={marketData} activeCoins={activeCoins} />
    </div>
  );
}
