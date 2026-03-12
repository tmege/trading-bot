import { useState, useEffect, useCallback } from 'react';

const FALLBACK_COINS = ['ETH', 'BTC', 'SOL', 'DOGE', 'HYPE', 'PUMP'];

export default function useActiveCoins() {
  const [coins, setCoins] = useState(FALLBACK_COINS);

  const refresh = useCallback(async () => {
    try {
      const res = await window.api.config.read();
      if (!res.ok) return;

      const active = res.config?.strategies?.active;
      if (!active || !Array.isArray(active)) return;

      const coinSet = new Set();
      for (const entry of active) {
        if (Array.isArray(entry.coins)) {
          for (const c of entry.coins) coinSet.add(c);
        }
      }

      if (coinSet.size > 0) {
        setCoins([...coinSet].sort());
      }
    } catch (_) {}
  }, []);

  useEffect(() => {
    refresh();
    const id = setInterval(refresh, 30000);
    return () => clearInterval(id);
  }, [refresh]);

  return coins;
}
