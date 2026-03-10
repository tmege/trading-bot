import React, { useState } from 'react';
import { Play, Square, Loader2 } from 'lucide-react';

export default function BotControls({ botStatus }) {
  const [loading, setLoading] = useState(false);

  async function handleStart() {
    setLoading(true);
    try {
      await window.api.bot.start();
    } catch (_) {}
    setLoading(false);
  }

  async function handleStop() {
    setLoading(true);
    try {
      await window.api.bot.stop();
    } catch (_) {}
    setLoading(false);
  }

  return (
    <div className="flex items-center gap-3">
      {botStatus.running ? (
        <button
          onClick={handleStop}
          disabled={loading}
          className="flex items-center gap-2 px-4 py-2 bg-loss/20 text-loss border border-loss/30 rounded-lg text-sm font-medium hover:bg-loss/30 transition-colors disabled:opacity-50"
        >
          {loading ? <Loader2 size={14} className="animate-spin" /> : <Square size={14} />}
          Stop Bot
        </button>
      ) : (
        <button
          onClick={handleStart}
          disabled={loading}
          className="flex items-center gap-2 px-4 py-2 bg-profit/20 text-profit border border-profit/30 rounded-lg text-sm font-medium hover:bg-profit/30 transition-colors disabled:opacity-50"
        >
          {loading ? <Loader2 size={14} className="animate-spin" /> : <Play size={14} />}
          Start Bot
        </button>
      )}

      <div className="flex items-center gap-2">
        <span className={`w-2.5 h-2.5 rounded-full ${
          botStatus.running ? 'bg-profit animate-pulse' : 'bg-gray-600'
        }`} />
        <span className={`text-sm font-medium ${
          botStatus.running ? 'text-profit' : 'text-gray-500'
        }`}>
          {botStatus.running ? 'Running' : 'Stopped'}
        </span>
      </div>
    </div>
  );
}
