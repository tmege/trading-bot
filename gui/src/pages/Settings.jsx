import React, { useState, useEffect, useCallback, useRef } from 'react';
import { Settings as SettingsIcon, ToggleLeft, ToggleRight, Plus, X, AlertTriangle } from 'lucide-react';
import ConfirmModal from '../components/ConfirmModal';

const POPULAR_COINS = ['ETH', 'BTC', 'SOL', 'DOGE', 'HYPE', 'PUMP', 'AVAX', 'ARB', 'OP', 'MATIC', 'LINK', 'UNI', 'AAVE', 'MKR', 'SNX', 'PEPE', 'WIF', 'BONK', 'JUP', 'TIA', 'SEI', 'SUI', 'APT', 'INJ', 'FTM', 'NEAR', 'ATOM', 'DOT', 'ADA', 'XRP', 'LTC', 'BCH'];

const COIN_RE = /^[A-Z]{2,10}$/;

export default function Settings({ paperMode, onPaperModeChange, botStatus, addToast, closeModalRef }) {
  const [config, setConfig] = useState(null);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [addingCoin, setAddingCoin] = useState(null);
  const [newCoin, setNewCoin] = useState('');
  const [showLiveConfirm, setShowLiveConfirm] = useState(false);
  const [needsRestart, setNeedsRestart] = useState(false);
  const prevRunning = useRef(null);

  // Input validation states
  const [coinError, setCoinError] = useState('');
  const [riskErrors, setRiskErrors] = useState({});

  // Register modal close callback
  useEffect(() => {
    if (closeModalRef) {
      closeModalRef.current = () => setShowLiveConfirm(false);
      return () => { closeModalRef.current = null; };
    }
  }, [closeModalRef]);

  // Clear needsRestart when bot restarts (stopped → running transition)
  useEffect(() => {
    if (prevRunning.current === false && botStatus?.running === true) {
      setNeedsRestart(false);
    }
    prevRunning.current = botStatus?.running ?? false;
  }, [botStatus?.running]);

  const loadConfig = useCallback(async () => {
    try {
      const res = await window.api.config.read();
      if (res.ok) setConfig(res.config);
    } catch (_) {}
  }, []);

  useEffect(() => { loadConfig(); }, [loadConfig]);

  async function saveConfig(updated, flagRestart = true) {
    setSaving(true);
    setSaved(false);
    try {
      const res = await window.api.config.write(updated);
      if (res.ok) {
        setConfig(updated);
        setSaved(true);
        if (flagRestart) setNeedsRestart(true);
        setTimeout(() => setSaved(false), 2000);
      }
    } catch (_) {}
    setSaving(false);
  }

  // Check if a coin is already used by another strategy
  function isCoinTaken(coin, excludeStratIdx) {
    if (!config?.strategies?.active) return null;
    for (let i = 0; i < config.strategies.active.length; i++) {
      if (i === excludeStratIdx) continue;
      const entry = config.strategies.active[i];
      if (entry.coins && entry.coins.includes(coin)) {
        return entry.file;
      }
    }
    return null;
  }

  function toggleCoin(stratIdx, coin) {
    if (!config) return;
    const updated = JSON.parse(JSON.stringify(config));
    const entry = updated.strategies.active[stratIdx];
    if (!entry.coins) return;

    const idx = entry.coins.indexOf(coin);
    if (idx !== -1) {
      if (entry.coins.length <= 1) return;
      entry.coins.splice(idx, 1);
    } else {
      const owner = isCoinTaken(coin, stratIdx);
      if (owner) {
        if (addToast) addToast('error', `${coin} is already used by ${owner}`);
        return;
      }
      entry.coins.push(coin);
    }
    saveConfig(updated);
  }

  function addCoin(stratIdx) {
    if (!config || !newCoin.trim()) return;
    const coin = newCoin.trim().toUpperCase();

    if (!COIN_RE.test(coin)) {
      setCoinError('2-10 uppercase letters only');
      return;
    }

    const owner = isCoinTaken(coin, stratIdx);
    if (owner) {
      setCoinError(`Used by ${owner}`);
      return;
    }
    setCoinError('');

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

  function handlePaperToggle() {
    if (!config) return;
    const isPaper = !!config.mode?.paper_trading;

    if (isPaper) {
      // Paper → Live: show confirmation
      setShowLiveConfirm(true);
    } else {
      // Live → Paper: safe, no confirmation needed
      const updated = JSON.parse(JSON.stringify(config));
      if (!updated.mode) updated.mode = {};
      updated.mode.paper_trading = true;
      saveConfig(updated);
      if (onPaperModeChange) onPaperModeChange(true);
      if (addToast) addToast('info', 'Switched to PAPER mode. Restart bot to apply.');
    }
  }

  function confirmLiveSwitch() {
    setShowLiveConfirm(false);
    const updated = JSON.parse(JSON.stringify(config));
    if (!updated.mode) updated.mode = {};
    updated.mode.paper_trading = false;
    saveConfig(updated);
    if (onPaperModeChange) onPaperModeChange(false);
    if (addToast) addToast('warning', 'Switched to LIVE mode. Restart bot to apply.');
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
    <div className={`flex-1 overflow-auto p-6 ${saving ? 'opacity-75 pointer-events-none' : ''}`}>
      <div className="max-w-2xl mx-auto">
        {/* Confirm Paper → Live modal */}
        <ConfirmModal
          open={showLiveConfirm}
          title="Switch to LIVE trading?"
          message="This will enable real orders on Hyperliquid with real money at risk. Make sure you understand the risks before proceeding."
          confirmLabel="Switch to LIVE"
          confirmVariant="danger"
          onConfirm={confirmLiveSwitch}
          onCancel={() => setShowLiveConfirm(false)}
        />

        {/* Header */}
        <div className="flex items-center gap-3 mb-6">
          <SettingsIcon size={20} className="text-accent" />
          <h1 className="text-lg font-semibold text-white">Settings</h1>
        </div>

        {/* Restart notice */}
        {needsRestart && (
          <div className="mb-4 px-3 py-2 bg-yellow-400/10 border border-yellow-400/30 rounded-lg text-xs text-yellow-400 flex items-center gap-2">
            <AlertTriangle size={14} />
            Restart the bot to apply configuration changes.
          </div>
        )}

        {/* Save indicator */}
        {saved && (
          <div className="mb-4 px-3 py-2 bg-profit/10 border border-profit/30 rounded-lg text-xs text-profit">
            Configuration saved.
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
                      {/* Per-strategy paper/live badge */}
                      {(() => {
                        const isPaper = entry.paper_mode != null
                          ? entry.paper_mode
                          : !!config.mode?.paper_trading;
                        return (
                          <span className={`px-2 py-0.5 rounded text-[10px] font-bold tracking-wider ${
                            isPaper
                              ? 'bg-yellow-500/20 text-yellow-400 border border-yellow-500/30'
                              : 'bg-profit/20 text-profit border border-profit/30'
                          }`}>
                            {isPaper ? 'PAPER' : 'LIVE'}
                          </span>
                        );
                      })()}
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
                          {POPULAR_COINS.filter(c => !entry.coins.includes(c)).slice(0, 12).map(coin => {
                            const takenBy = isCoinTaken(coin, stratIdx);
                            return (
                              <button
                                key={coin}
                                disabled={!!takenBy}
                                title={takenBy ? `Used by ${takenBy}` : undefined}
                                onClick={() => {
                                  if (takenBy) return;
                                  const updated = JSON.parse(JSON.stringify(config));
                                  updated.strategies.active[stratIdx].coins.push(coin);
                                  saveConfig(updated);
                                }}
                                className={`flex items-center gap-1 px-2 py-1 border rounded text-[11px] transition-colors ${
                                  takenBy
                                    ? 'bg-surface-bg border-surface-border text-gray-700 cursor-not-allowed opacity-50'
                                    : 'bg-surface-hover border-surface-border text-gray-500 hover:text-white hover:border-accent/50'
                                }`}
                              >
                                <Plus size={10} />
                                {coin}
                              </button>
                            );
                          })}

                          {/* Custom add */}
                          {addingCoin === stratIdx ? (
                            <form
                              onSubmit={(e) => { e.preventDefault(); addCoin(stratIdx); }}
                              className="flex items-center gap-1"
                            >
                              <div>
                                <input
                                  autoFocus
                                  value={newCoin}
                                  onChange={e => { setNewCoin(e.target.value); setCoinError(''); }}
                                  placeholder="COIN"
                                  maxLength={10}
                                  className={`w-20 px-2 py-1 bg-surface-bg border rounded text-[11px] text-white placeholder-gray-600 outline-none ${
                                    coinError ? 'border-loss' : 'border-accent/50 focus:border-accent'
                                  }`}
                                />
                                {coinError && (
                                  <div className="text-[9px] text-loss mt-0.5">{coinError}</div>
                                )}
                              </div>
                              <button
                                type="submit"
                                className="px-2 py-1 bg-accent/20 text-accent rounded text-[11px] hover:bg-accent/30"
                              >
                                Add
                              </button>
                              <button
                                type="button"
                                onClick={() => { setAddingCoin(null); setNewCoin(''); setCoinError(''); }}
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

                  {/* Per-strategy paper mode toggle */}
                  {!config.mode?.paper_trading && (
                    <div className="px-4 py-3 border-t border-surface-border">
                      <div className="flex items-center justify-between">
                        <div>
                          <span className="text-xs text-gray-400">Paper mode</span>
                          <p className="text-[10px] text-gray-600 mt-0.5">
                            Override global mode for this strategy only.
                          </p>
                        </div>
                        <button
                          onClick={() => {
                            const updated = JSON.parse(JSON.stringify(config));
                            const e = updated.strategies.active[stratIdx];
                            if (e.paper_mode) {
                              delete e.paper_mode;
                              delete e.paper_balance;
                            } else {
                              e.paper_mode = true;
                              if (!e.paper_balance) e.paper_balance = config.mode?.paper_initial_balance || 500;
                            }
                            saveConfig(updated);
                          }}
                        >
                          {entry.paper_mode ? (
                            <ToggleRight size={24} className="text-yellow-400" />
                          ) : (
                            <ToggleLeft size={24} className="text-gray-600" />
                          )}
                        </button>
                      </div>
                      {entry.paper_mode && (
                        <div className="mt-2 flex items-center gap-2">
                          <label className="text-[10px] text-gray-500">Balance:</label>
                          <input
                            type="number"
                            min="10"
                            max="100000"
                            step="10"
                            value={entry.paper_balance ?? config.mode?.paper_initial_balance ?? 500}
                            onChange={(e) => {
                              const val = parseFloat(e.target.value);
                              if (isNaN(val) || val < 10 || val > 100000) return;
                              const updated = JSON.parse(JSON.stringify(config));
                              updated.strategies.active[stratIdx].paper_balance = val;
                              saveConfig(updated);
                            }}
                            className="w-24 px-2 py-1 bg-surface-bg border border-yellow-500/30 rounded text-xs text-white font-mono outline-none focus:border-yellow-400"
                          />
                          <span className="text-[10px] text-gray-600">USDC</span>
                        </div>
                      )}
                    </div>
                  )}

                  {/* Legacy mode — offer to enable multi-coin */}
                  {!entry.coins && (
                    <div className="p-4 flex items-center gap-3">
                      <span className="text-xs text-gray-600">Legacy mode (single instance)</span>
                      <button
                        className="px-3 py-1 text-xs bg-accent/20 text-accent border border-accent/30 rounded hover:bg-accent/30"
                        onClick={() => {
                          const updated = JSON.parse(JSON.stringify(config));
                          const available = ["ETH", "BTC"].filter(c => !isCoinTaken(c, stratIdx));
                          if (available.length === 0) {
                            if (addToast) addToast('error', 'ETH and BTC are already used by other strategies');
                            return;
                          }
                          updated.strategies.active[stratIdx].coins = available;
                          saveConfig(updated);
                        }}
                      >
                        Enable multi-coin
                      </button>
                    </div>
                  )}
                </div>
              ))}
            </div>
          )}
        </section>

        {/* Risk Parameters (editable, %-based) */}
        <section className="mb-8">
          <h2 className="text-xs text-gray-500 uppercase tracking-wider mb-3">Risk Parameters (% of account)</h2>
          <div className="bg-surface-card border border-surface-border rounded-lg p-4">
            <div className="grid grid-cols-2 gap-4">
              {[
                { key: 'daily_loss_pct',      label: 'Max daily loss',      unit: '%', step: 1, min: 1, max: 50 },
                { key: 'emergency_close_pct',  label: 'Emergency close',     unit: '%', step: 1, min: 1, max: 40 },
                { key: 'max_position_pct',     label: 'Max position size',   unit: '%', step: 10, min: 10, max: 500 },
                { key: 'max_leverage',         label: 'Max leverage',        unit: 'x', step: 1, min: 1, max: 10 },
              ].map(({ key, label, unit, step, min, max }) => {
                const hasError = !!riskErrors[key];
                return (
                  <div key={key}>
                    <label className="text-[10px] text-gray-500 uppercase block mb-1">{label}</label>
                    <div className="flex items-center gap-2">
                      <input
                        type="number"
                        className={`w-20 bg-surface-bg border rounded px-2 py-1 text-xs text-white font-mono text-right ${
                          hasError ? 'border-loss' : 'border-surface-border'
                        }`}
                        value={config.risk?.[key] ?? ''}
                        step={step}
                        min={min}
                        max={max}
                        onChange={(e) => {
                          const val = parseFloat(e.target.value);
                          if (isNaN(val)) {
                            setRiskErrors(prev => ({ ...prev, [key]: `Enter a number` }));
                            return;
                          }
                          if (val < min || val > max) {
                            setRiskErrors(prev => ({ ...prev, [key]: `${min}-${max}` }));
                            return;
                          }
                          setRiskErrors(prev => { const n = { ...prev }; delete n[key]; return n; });
                          const updated = JSON.parse(JSON.stringify(config));
                          if (!updated.risk) updated.risk = {};
                          updated.risk[key] = val;
                          saveConfig(updated);
                        }}
                      />
                      <span className="text-xs text-gray-500">{unit}</span>
                    </div>
                    {hasError && (
                      <div className="text-[9px] text-loss mt-0.5">{riskErrors[key]}</div>
                    )}
                  </div>
                );
              })}
            </div>
            <p className="text-[10px] text-gray-600 mt-3">
              All limits scale automatically with your account balance.
            </p>
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
                onClick={handlePaperToggle}
                className="shrink-0 ml-4"
              >
                {config.mode?.paper_trading ? (
                  <ToggleRight size={32} className="text-yellow-400" />
                ) : (
                  <ToggleLeft size={32} className="text-gray-600" />
                )}
              </button>
            </div>

            {/* Paper bankroll */}
            {config.mode?.paper_trading && (
              <div className="mt-3 pt-3 border-t border-yellow-500/20">
                <label className="text-xs text-gray-500 block mb-1.5">Starting Bankroll (USDC)</label>
                <div className="flex items-center gap-2">
                  <input
                    type="number"
                    min="10"
                    max="100000"
                    step="10"
                    value={config.mode?.paper_initial_balance ?? 100}
                    onChange={(e) => {
                      const val = parseFloat(e.target.value);
                      if (isNaN(val) || val < 10 || val > 100000) return;
                      const updated = JSON.parse(JSON.stringify(config));
                      if (!updated.mode) updated.mode = {};
                      updated.mode.paper_initial_balance = val;
                      saveConfig(updated);
                    }}
                    className="w-32 px-3 py-1.5 bg-surface-bg border border-yellow-500/30 rounded-lg text-sm text-white font-mono outline-none focus:border-yellow-400"
                  />
                  <span className="text-xs text-gray-600">10 - 100,000</span>
                </div>
              </div>
            )}
          </div>
        </section>
      </div>
    </div>
  );
}
