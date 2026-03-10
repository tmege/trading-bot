import { useState, useEffect, useCallback, useRef } from 'react';

let nextId = 1;

export default function useTradeNotifications() {
  const [notifications, setNotifications] = useState([]);
  const seenHashes = useRef(new Set());

  const dismiss = useCallback((id) => {
    setNotifications((prev) => prev.filter((n) => n.id !== id));
  }, []);

  useEffect(() => {
    if (!window.api?.ws?.onUser) return;

    const cleanup = window.api.ws.onUser((data) => {
      // Hyperliquid userEvents: { fills: [...] } or array of events
      const events = Array.isArray(data) ? data : [data];

      for (const evt of events) {
        const fills = evt.fills || [];
        for (const fill of fills) {
          // Deduplicate by hash
          if (fill.hash && seenHashes.current.has(fill.hash)) continue;
          if (fill.hash) seenHashes.current.add(fill.hash);

          // Keep set bounded
          if (seenHashes.current.size > 500) {
            const arr = [...seenHashes.current];
            seenHashes.current = new Set(arr.slice(-250));
          }

          const id = nextId++;
          const isBuy = fill.side === 'B';
          const closedPnl = parseFloat(fill.closedPnl || '0');
          const isClose = Math.abs(closedPnl) > 0.0001;

          const notif = {
            id,
            coin: fill.coin || '???',
            side: isBuy ? 'BUY' : 'SELL',
            isBuy,
            price: parseFloat(fill.px || '0'),
            size: parseFloat(fill.sz || '0'),
            fee: parseFloat(fill.fee || '0'),
            closedPnl,
            isClose,
            dir: fill.dir || '',
            time: fill.time || Date.now(),
          };

          setNotifications((prev) => [notif, ...prev].slice(0, 8));

          // Auto dismiss after 6s
          setTimeout(() => dismiss(id), 6000);
        }
      }
    });

    return cleanup;
  }, [dismiss]);

  return { notifications, dismiss };
}
