import { useState, useEffect, useCallback } from 'react';

export default function useBacktest() {
  const [results, setResults] = useState(null);
  const [progress, setProgress] = useState(null);
  const [running, setRunning] = useState(false);
  const [error, setError] = useState(null);

  useEffect(() => {
    let cleanup;
    try {
      cleanup = window.api.backtest.onProgress((data) => {
        setProgress(data);
      });
    } catch (_) {
      // api not available
    }
    return () => cleanup?.();
  }, []);

  const run = useCallback(async (params) => {
    setRunning(true);
    setError(null);
    setResults(null);
    setProgress({ status: 'starting' });

    try {
      const res = await window.api.backtest.run(params);
      if (res.ok) {
        setResults(res.results);
      } else {
        setError(res.error || 'Backtest failed');
      }
    } catch (err) {
      setError(err.message);
    } finally {
      setRunning(false);
      setProgress(null);
    }
  }, []);

  return { results, progress, running, error, run };
}
