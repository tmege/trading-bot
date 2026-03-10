import React from 'react';
import { FileCode, ToggleLeft, ToggleRight } from 'lucide-react';

const ROLE_COLORS = {
  primary:   'bg-accent/20 text-accent',
  secondary: 'bg-purple-500/20 text-purple-400',
  inactive:  'bg-gray-700/30 text-gray-500',
};

export default function StrategyList({ strategies, selected, onSelect, onToggle }) {
  return (
    <div className="space-y-2">
      {strategies.map((s) => (
        <div
          key={s.file}
          onClick={() => onSelect(s.file)}
          className={`p-3 rounded-lg border cursor-pointer transition-colors ${
            selected === s.file
              ? 'border-accent bg-accent/5'
              : 'border-surface-border bg-surface-card hover:bg-surface-hover'
          }`}
        >
          <div className="flex items-center justify-between mb-2">
            <div className="flex items-center gap-2">
              <FileCode size={14} className="text-gray-500" />
              <span className="text-sm font-medium text-white">{s.file}</span>
            </div>
            <button
              onClick={(e) => {
                e.stopPropagation();
                onToggle(s.file, !s.active);
              }}
              className="text-gray-400 hover:text-white transition-colors"
            >
              {s.active ? (
                <ToggleRight size={20} className="text-profit" />
              ) : (
                <ToggleLeft size={20} className="text-gray-600" />
              )}
            </button>
          </div>
          <div className="flex items-center gap-2 flex-wrap">
            <span className={`px-2 py-0.5 rounded text-xs font-medium ${
              ROLE_COLORS[s.role] || ROLE_COLORS.inactive
            }`}>
              {s.role.toUpperCase()}
            </span>
            <span className={`text-xs ${s.active ? 'text-profit' : 'text-gray-600'}`}>
              {s.active ? 'Active' : 'Inactive'}
            </span>
            {s.coins && s.coins.length > 0 && (
              <div className="flex gap-1">
                {s.coins.map(c => (
                  <span key={c} className="px-1.5 py-0.5 bg-accent/10 text-accent/80 rounded text-[10px] font-medium">
                    {c}
                  </span>
                ))}
              </div>
            )}
          </div>
        </div>
      ))}
    </div>
  );
}
