import React from 'react';
import { LayoutDashboard, BarChart3, Code2, FlaskConical, Settings } from 'lucide-react';
import logoSrc from '../assets/logo.png';

const NAV = [
  { id: 'dashboard',  label: 'Dashboard',  icon: LayoutDashboard },
  { id: 'market',     label: 'Market',     icon: BarChart3 },
  { id: 'strategies', label: 'Strategies', icon: Code2 },
  { id: 'backtest',   label: 'Backtest',   icon: FlaskConical },
  { id: 'settings',   label: 'Settings',   icon: Settings },
];

export default function Sidebar({ currentPage, onNavigate, botStatus }) {
  return (
    <aside className="w-48 bg-surface-card border-r border-surface-border flex flex-col shrink-0">
      <div className="px-4 py-3 border-b border-surface-border flex items-center gap-2">
        <span className={`w-2 h-2 rounded-full ${botStatus.running ? 'bg-profit animate-pulse' : 'bg-gray-600'}`} />
        <span className={`text-xs font-medium ${botStatus.running ? 'text-profit' : 'text-gray-500'}`}>
          {botStatus.running ? 'LIVE' : 'OFFLINE'}
        </span>
      </div>

      <nav className="flex-1 py-2">
        {NAV.map(({ id, label, icon: Icon }) => (
          <button
            key={id}
            onClick={() => onNavigate(id)}
            className={`w-full flex items-center gap-3 px-4 py-2.5 text-sm transition-colors ${
              currentPage === id
                ? 'text-white bg-surface-hover border-r-2 border-accent'
                : 'text-gray-400 hover:text-gray-200 hover:bg-surface-hover'
            }`}
          >
            <Icon size={16} />
            {label}
          </button>
        ))}
      </nav>

      <div className="px-2 py-3 border-t border-surface-border flex items-center justify-center">
        <img src={logoSrc} alt="Logo" className="w-full opacity-40" />
      </div>
    </aside>
  );
}
