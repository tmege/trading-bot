import { useState, useEffect, useCallback } from 'react';

const defaultAccount = {
  dailyPnl: 0, dailyFees: 0, dailyTrades: 0, totalTrades: 0, balance: 0,
};

export default function useLiveData() {
  const [positions, setPositions] = useState([]);
  const [trades, setTrades] = useState([]);
  const [account, setAccount] = useState(defaultAccount);

  const refresh = useCallback(async () => {
    try {
      const [posRes, tradeRes, accRes] = await Promise.all([
        window.api.db.positions(),
        window.api.db.trades(30),
        window.api.db.account(),
      ]);

      if (posRes.ok) setPositions(posRes.positions);
      if (tradeRes.ok) setTrades(tradeRes.trades);
      if (accRes.ok && accRes.account) setAccount(accRes.account);
    } catch (_) {
      // api not available
    }
  }, []);

  useEffect(() => {
    refresh();
    const id = setInterval(refresh, 3000);
    return () => clearInterval(id);
  }, [refresh]);

  return { positions, trades, account, refresh };
}
