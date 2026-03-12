import { useState, useEffect, useRef } from 'react';

const DEFAULT_COINS = ['ETH', 'BTC', 'SOL', 'DOGE', 'HYPE', 'PUMP'];
const STALE_TIMEOUT_MS = 10000;

export default function usePrices(coins) {
  const coinList = coins || DEFAULT_COINS;
  const [prices, setPrices] = useState({});
  const [stale, setStale] = useState(false);
  const lastUpdate = useRef(Date.now());
  const staleTimer = useRef(null);

  useEffect(() => {
    if (!window.api?.ws) return;

    function resetStaleTimer() {
      lastUpdate.current = Date.now();
      setStale(false);
      clearTimeout(staleTimer.current);
      staleTimer.current = setTimeout(() => setStale(true), STALE_TIMEOUT_MS);
    }

    // Start stale timer
    staleTimer.current = setTimeout(() => setStale(true), STALE_TIMEOUT_MS);

    const cleanup = window.api.ws.onPrices((mids) => {
      const filtered = {};
      for (const coin of coinList) {
        if (mids[coin]) filtered[coin] = parseFloat(mids[coin]);
      }
      setPrices(filtered);
      resetStaleTimer();
    });

    return () => {
      cleanup();
      clearTimeout(staleTimer.current);
    };
  }, [JSON.stringify(coinList)]);

  return { prices, stale };
}
