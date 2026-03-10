import { useState, useEffect } from 'react';

const COINS = ['ETH', 'BTC', 'SOL', 'DOGE', 'HYPE', 'PUMP'];

export default function usePrices() {
  const [prices, setPrices] = useState({});

  useEffect(() => {
    if (!window.api?.ws) return;

    const cleanup = window.api.ws.onPrices((mids) => {
      const filtered = {};
      for (const coin of COINS) {
        if (mids[coin]) filtered[coin] = parseFloat(mids[coin]);
      }
      setPrices(filtered);
    });

    return cleanup;
  }, []);

  return prices;
}
