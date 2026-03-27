import React from 'react';
import { LayoutDashboard, BarChart3, Code2, FlaskConical, Settings } from 'lucide-react';
import logoSrc from '../assets/logo.png';

const NAV = [
  { id: 'dashboard',  label: 'Dashboard',  icon: LayoutDashboard, shortcut: '1' },
  { id: 'market',     label: 'Market',     icon: BarChart3,       shortcut: '2' },
  { id: 'strategies', label: 'Strategies', icon: Code2,           shortcut: '3' },
  { id: 'backtest',   label: 'Backtest',   icon: FlaskConical,    shortcut: '4' },
  { id: 'settings',   label: 'Settings',   icon: Settings,        shortcut: '5' },
];

const IS_MAC = typeof navigator !== 'undefined' && /Mac/.test(navigator.userAgent);
const MOD = IS_MAC ? '\u2318' : 'Ctrl+';

export default function Sidebar({ currentPage, onNavigate, botStatus, collapsed }) {
  return (
    <aside className={`${collapsed ? 'w-14' : 'w-48'} bg-surface-card border-r border-surface-border flex flex-col shrink-0 transition-all duration-200`}>
      <div className={`${collapsed ? 'px-2 justify-center' : 'px-4'} py-3 border-b border-surface-border flex items-center gap-2`}>
        <span className={`w-2 h-2 rounded-full ${botStatus.running ? 'bg-profit animate-pulse' : 'bg-gray-600'}`} />
        {!collapsed && (
          <span className={`text-xs font-medium ${botStatus.running ? 'text-profit' : 'text-gray-500'}`}>
            {botStatus.running ? 'RUNNING' : 'OFFLINE'}
          </span>
        )}
      </div>

      <nav className="flex-1 py-2">
        {NAV.map(({ id, label, icon: Icon, shortcut }) => (
          <button
            key={id}
            onClick={() => onNavigate(id)}
            title={collapsed ? `${label} (${MOD}${shortcut})` : undefined}
            className={`w-full flex items-center gap-3 ${collapsed ? 'justify-center px-2' : 'px-4'} py-2.5 text-sm transition-colors ${
              currentPage === id
                ? 'text-white bg-surface-hover border-r-2 border-accent'
                : 'text-gray-400 hover:text-gray-200 hover:bg-surface-hover'
            }`}
          >
            <Icon size={16} />
            {!collapsed && (
              <>
                <span className="flex-1 text-left">{label}</span>
                <span className="text-[10px] text-gray-600 font-mono">{MOD}{shortcut}</span>
              </>
            )}
          </button>
        ))}
      </nav>

      {!collapsed && (
        <div className="px-2 py-3 border-t border-surface-border flex items-center justify-center">
          <img src={logoSrc} alt="Logo" className="w-full opacity-40" />
        </div>
      )}
    </aside>
  );
}
