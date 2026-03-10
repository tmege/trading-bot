import React, { useState, useEffect, useCallback } from 'react';
import StrategyList from '../components/StrategyList';
import CodeViewer from '../components/CodeViewer';

export default function Strategies() {
  const [strategies, setStrategies] = useState([]);
  const [selected, setSelected] = useState(null);
  const [code, setCode] = useState(null);

  const loadStrategies = useCallback(async () => {
    try {
      const res = await window.api.strategies.list();
      if (res.ok) setStrategies(res.strategies);
    } catch (_) {}
  }, []);

  useEffect(() => {
    loadStrategies();
  }, [loadStrategies]);

  async function handleSelect(filename) {
    setSelected(filename);
    try {
      const res = await window.api.strategies.read(filename);
      if (res.ok) setCode(res.content);
    } catch (_) {}
  }

  async function handleToggle(filename, enabled) {
    try {
      await window.api.strategies.toggle(filename, enabled);
      await loadStrategies();
    } catch (_) {}
  }

  return (
    <div className="flex-1 flex overflow-hidden">
      {/* Left panel: strategy list */}
      <div className="w-2/5 p-4 overflow-auto border-r border-surface-border">
        <h2 className="text-sm font-bold text-white mb-4 uppercase tracking-wider">
          Strategies
        </h2>
        <StrategyList
          strategies={strategies}
          selected={selected}
          onSelect={handleSelect}
          onToggle={handleToggle}
        />
      </div>

      {/* Right panel: code viewer */}
      <div className="flex-1 p-4 flex flex-col">
        <CodeViewer code={code} filename={selected} />
      </div>
    </div>
  );
}
