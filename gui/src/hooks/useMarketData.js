import { useState, useEffect, useRef } from 'react';

const POLL_MS = 120_000; // 120s — aligned with main process cache

export default function useMarketData() {
  const [data, setData] = useState(null);
  const timer = useRef(null);

  useEffect(() => {
    let mounted = true;

    async function load() {
      try {
        const res = await window.api.market.data();
        if (res.ok && mounted) setData(res.data);
      } catch (_) {}
    }

    load();
    timer.current = setInterval(load, POLL_MS);

    return () => {
      mounted = false;
      clearInterval(timer.current);
    };
  }, []);

  return data;
}
