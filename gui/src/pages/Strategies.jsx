import React, { useState, useEffect, useCallback } from 'react';
import StrategyList from '../components/StrategyList';
import CodeViewer from '../components/CodeViewer';

export default function Strategies() {
  const [strategies, setStrategies] = useState([]);
  const [selected, setSelected] = useState(null);
  const [code, setCode] = useState(null);
  const [loading, setLoading] = useState(true);
  const [togglingFile, setTogglingFile] = useState(null);
  const [stratPnl, setStratPnl] = useState({});
  const [stratDetails, setStratDetails] = useState({});

  const loadStrategies = useCallback(async () => {
    try {
      const res = await window.api.strategies.list();
      if (res.ok) setStrategies(res.strategies);
    } catch (_) {}
    setLoading(false);
  }, []);

  const loadPnl = useCallback(async () => {
    try {
      if (!window.api?.db?.strategyPnl) return;
      const res = await window.api.db.strategyPnl();
      if (res.ok) setStratPnl(res.pnl || {});
    } catch (_) {}
  }, []);

  const loadDetails = useCallback(async () => {
    try {
      if (!window.api?.db?.strategyDetails) return;
      const res = await window.api.db.strategyDetails();
      if (res.ok) setStratDetails(res.details || {});
    } catch (_) {}
  }, []);

  useEffect(() => {
    loadStrategies();
    loadPnl();
    loadDetails();
  }, [loadStrategies, loadPnl, loadDetails]);

  async function handleSelect(filename) {
    setSelected(filename);
    try {
      const res = await window.api.strategies.read(filename);
      if (res.ok) setCode(res.content);
    } catch (_) {}
  }

  async function handleToggle(filename, enabled) {
    setTogglingFile(filename);
    try {
      await window.api.strategies.toggle(filename, enabled);
      await loadStrategies();
    } catch (_) {}
    setTogglingFile(null);
  }

  return (
    <div className="flex-1 flex flex-col lg:flex-row overflow-hidden">
      {/* Left panel: strategy list */}
      <div className="w-full lg:w-2/5 p-4 overflow-auto border-b lg:border-b-0 lg:border-r border-surface-border">
        <h2 className="text-sm font-bold text-white mb-4 uppercase tracking-wider">
          Strategies
        </h2>
        <StrategyList
          strategies={strategies}
          selected={selected}
          onSelect={handleSelect}
          onToggle={handleToggle}
          stratPnl={stratPnl}
          stratDetails={stratDetails}
          loading={loading}
          togglingFile={togglingFile}
        />
      </div>

      {/* Right panel: code viewer */}
      <div className="flex-1 p-4 flex flex-col">
        <CodeViewer code={code} filename={selected} />
      </div>
    </div>
  );
}
