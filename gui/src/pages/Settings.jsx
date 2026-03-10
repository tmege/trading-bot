import React, { useState, useEffect, useCallback } from 'react';
import { Settings as SettingsIcon, ToggleLeft, ToggleRight, Plus, X, RefreshCw } from 'lucide-react';
import SyncPanel from '../components/SyncPanel';

const POPULAR_COINS = ['ETH', 'BTC', 'SOL', 'DOGE', 'HYPE', 'PUMP', 'AVAX', 'ARB', 'OP', 'MATIC', 'LINK', 'UNI', 'AAVE', 'MKR', 'SNX', 'PEPE', 'WIF', 'BONK', 'JUP', 'TIA', 'SEI', 'SUI', 'APT', 'INJ', 'FTM', 'NEAR', 'ATOM', 'DOT', 'ADA', 'XRP', 'LTC', 'BCH'];

export default function Settings({ paperMode, onPaperModeChange }) {
  const [config, setConfig] = useState(null);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [addingCoin, setAddingCoin] = useState(null); // index of strategy being edited
  const [newCoin, setNewCoin] = useState('');

  const loadConfig = useCallback(async () => {
    try {
      const res = await window.api.config.read();
      if (res.ok) setConfig(res.config);
    } catch (_) {}
  }, []);

  useEffect(() => { loadConfig(); }, [loadConfig]);

  async function saveConfig(updated) {
    setSaving(true);
    setSaved(false);
    try {
      const res = await window.api.config.write(updated);
      if (res.ok) {
        setConfig(updated);
        setSaved(true);
        setTimeout(() => setSaved(false), 2000);
      }
    } catch (_) {}
    setSaving(false);
  }

  function toggleCoin(stratIdx, coin) {
    if (!config) return;
    const updated = JSON.parse(JSON.stringify(config));
    const entry = updated.strategies.active[stratIdx];
    if (!entry.coins) return;

    const idx = entry.coins.indexOf(coin);
    if (idx !== -1) {
      // Don't allow removing the last coin
      if (entry.coins.length <= 1) return;
      entry.coins.splice(idx, 1);
    } else {
      entry.coins.push(coin);
    }
    saveConfig(updated);
  }

  function addCoin(stratIdx) {
    if (!config || !newCoin.trim()) return;
    const coin = newCoin.trim().toUpperCase();
    const updated = JSON.parse(JSON.stringify(config));
    const entry = updated.strategies.active[stratIdx];
    if (!entry.coins) entry.coins = [];
    if (entry.coins.includes(coin)) {
      setNewCoin('');
      setAddingCoin(null);
      return;
    }
    entry.coins.push(coin);
    setNewCoin('');
    setAddingCoin(null);
    saveConfig(updated);
  }

  function removeCoin(stratIdx, coin) {
    if (!config) return;
    const updated = JSON.parse(JSON.stringify(config));
    const entry = updated.strategies.active[stratIdx];
    if (!entry.coins || entry.coins.length <= 1) return;
    entry.coins = entry.coins.filter(c => c !== coin);
    saveConfig(updated);
  }

  if (!config) {
    return (
      <div className="flex-1 flex items-center justify-center text-gray-500 text-sm">
        Loading configuration...
      </div>
    );
  }

  const active = config.strategies?.active || [];

  return (
    <div className="flex-1 overflow-auto p-6">
      <div className="max-w-2xl mx-auto">
        {/* Header */}
        <div className="flex items-center justify-between mb-6">
          <div className="flex items-center gap-3">
            <SettingsIcon size={20} className="text-accent" />
            <h1 className="text-lg font-semibold text-white">Settings</h1>
          </div>
          <button
            onClick={loadConfig}
            className="flex items-center gap-1.5 px-3 py-1.5 text-xs text-gray-400 hover:text-white border border-surface-border rounded-lg hover:bg-surface-hover transition-colors"
          >
            <RefreshCw size={12} />
            Reload
          </button>
        </div>

        {/* Save indicator */}
        {saved && (
          <div className="mb-4 px-3 py-2 bg-profit/10 border border-profit/30 rounded-lg text-xs text-profit">
            Configuration saved. Restart the bot to apply changes.
          </div>
        )}

        {/* Active Strategies & Coins */}
        <section className="mb-8">
          <h2 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Active Coins</h2>
          <p className="text-xs text-gray-600 mb-4">
            Toggle coins on/off per strategy. Changes are saved to bot_config.json instantly.
          </p>

          {active.length === 0 ? (
            <div className="text-sm text-gray-600 p-4 border border-surface-border rounded-lg bg-surface-card">
              No active strategies configured.
            </div>
          ) : (
            <div className="space-y-4">
              {active.map((entry, stratIdx) => (
                <div
                  key={stratIdx}
                  className="bg-surface-card border border-surface-border rounded-lg overflow-hidden"
                >
                  {/* Strategy header */}
                  <div className="px-4 py-3 border-b border-surface-border flex items-center justify-between">
                    <div className="flex items-center gap-2">
                      <span className="text-sm font-medium text-white">{entry.file}</span>
                      <span className={`px-2 py-0.5 rounded text-[10px] font-medium ${
                        entry.role === 'primary'
                          ? 'bg-accent/20 text-accent'
                          : 'bg-purple-500/20 text-purple-400'
                      }`}>
                        {(entry.role || 'secondary').toUpperCase()}
                      </span>
                    </div>
                    <span className="text-xs text-gray-500">
                      {entry.coins ? `${entry.coins.length} coins` : 'legacy mode'}
                    </span>
                  </div>

                  {/* Coins grid */}
                  {entry.coins && (
                    <div className="p-4">
                      {/* Active coins */}
                      <div className="flex flex-wrap gap-2 mb-3">
                        {entry.coins.map(coin => (
                          <div
                            key={coin}
                            className="group flex items-center gap-1.5 px-3 py-1.5 bg-profit/10 border border-profit/30 rounded-lg text-sm text-profit cursor-pointer hover:bg-profit/20 transition-colors"
                            onClick={() => toggleCoin(stratIdx, coin)}
                          >
                            <ToggleRight size={14} />
                            <span className="font-medium">{coin}</span>
                            <button
                              onClick={(e) => {
                                e.stopPropagation();
                                removeCoin(stratIdx, coin);
                              }}
                              className="ml-1 opacity-0 group-hover:opacity-100 text-profit/60 hover:text-loss transition-all"
                            >
                              <X size={12} />
                            </button>
                          </div>
                        ))}
                      </div>

                      {/* Quick-add popular coins (not already active) */}
                      <div className="border-t border-surface-border pt-3">
                        <span className="text-[10px] text-gray-600 uppercase tracking-wider">Quick add</span>
                        <div className="flex flex-wrap gap-1.5 mt-2">
                          {POPULAR_COINS.filter(c => !entry.coins.includes(c)).slice(0, 12).map(coin => (
                            <button
                              key={coin}
                              onClick={() => {
                                const updated = JSON.parse(JSON.stringify(config));
                                updated.strategies.active[stratIdx].coins.push(coin);
                                saveConfig(updated);
                              }}
                              className="flex items-center gap-1 px-2 py-1 bg-surface-hover border border-surface-border rounded text-[11px] text-gray-500 hover:text-white hover:border-accent/50 transition-colors"
                            >
                              <Plus size={10} />
                              {coin}
                            </button>
                          ))}

                          {/* Custom add */}
                          {addingCoin === stratIdx ? (
                            <form
                              onSubmit={(e) => { e.preventDefault(); addCoin(stratIdx); }}
                              className="flex items-center gap-1"
                            >
                              <input
                                autoFocus
                                value={newCoin}
                                onChange={e => setNewCoin(e.target.value)}
                                placeholder="COIN"
                                maxLength={10}
                                className="w-20 px-2 py-1 bg-surface-bg border border-accent/50 rounded text-[11px] text-white placeholder-gray-600 outline-none focus:border-accent"
                              />
                              <button
                                type="submit"
                                className="px-2 py-1 bg-accent/20 text-accent rounded text-[11px] hover:bg-accent/30"
                              >
                                Add
                              </button>
                              <button
                                type="button"
                                onClick={() => { setAddingCoin(null); setNewCoin(''); }}
                                className="px-1 py-1 text-gray-500 hover:text-white"
                              >
                                <X size={12} />
                              </button>
                            </form>
                          ) : (
                            <button
                              onClick={() => setAddingCoin(stratIdx)}
                              className="flex items-center gap-1 px-2 py-1 border border-dashed border-surface-border rounded text-[11px] text-gray-600 hover:text-accent hover:border-accent/50 transition-colors"
                            >
                              <Plus size={10} />
                              Custom...
                            </button>
                          )}
                        </div>
                      </div>
                    </div>
                  )}

                  {/* Legacy mode (no coins array) */}
                  {!entry.coins && (
                    <div className="p-4 text-xs text-gray-600">
                      This strategy uses legacy mode (single file per coin). Add a <code className="text-gray-400">"coins"</code> array to enable multi-coin.
                    </div>
                  )}
                </div>
              ))}
            </div>
          )}
        </section>

        {/* Market Data Cache */}
        <section className="mb-8">
          <h2 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Market Data</h2>
          <SyncPanel />
        </section>

        {/* Risk overview (read-only) */}
        <section className="mb-8">
          <h2 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Risk Parameters</h2>
          <div className="bg-surface-card border border-surface-border rounded-lg p-4">
            <div className="grid grid-cols-2 gap-3">
              {config.risk && Object.entries(config.risk).map(([key, val]) => (
                <div key={key} className="flex justify-between items-center">
                  <span className="text-xs text-gray-500">{key.replace(/_/g, ' ')}</span>
                  <span className="text-xs text-white font-mono">{val}</span>
                </div>
              ))}
            </div>
          </div>
        </section>

        {/* Mode */}
        <section>
          <h2 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Trading Mode</h2>
          <div className={`bg-surface-card border rounded-lg p-4 ${
            config.mode?.paper_trading
              ? 'border-yellow-500/40'
              : 'border-surface-border'
          }`}>
            <div className="flex items-center justify-between">
              <div>
                <div className="flex items-center gap-2 mb-1">
                  <span className="text-sm font-medium text-white">Paper Trading</span>
                  <span className={`px-2 py-0.5 rounded text-[10px] font-bold tracking-wider ${
                    config.mode?.paper_trading
                      ? 'bg-yellow-500/20 text-yellow-400 border border-yellow-500/30'
                      : 'bg-profit/20 text-profit border border-profit/30'
                  }`}>
                    {config.mode?.paper_trading ? 'PAPER' : 'LIVE'}
                  </span>
                </div>
                <p className="text-xs text-gray-600">
                  {config.mode?.paper_trading
                    ? 'Simulated orders — no real money at risk.'
                    : 'Real orders on Hyperliquid — real money at risk.'}
                </p>
              </div>
              <button
                onClick={() => {
                  const updated = JSON.parse(JSON.stringify(config));
                  if (!updated.mode) updated.mode = {};
                  updated.mode.paper_trading = !updated.mode.paper_trading;
                  saveConfig(updated);
                  if (onPaperModeChange) onPaperModeChange(updated.mode.paper_trading);
                }}
                className="shrink-0 ml-4"
              >
                {config.mode?.paper_trading ? (
                  <ToggleRight size={32} className="text-yellow-400" />
                ) : (
                  <ToggleLeft size={32} className="text-gray-600" />
                )}
              </button>
            </div>
          </div>
        </section>
      </div>
    </div>
  );
}
