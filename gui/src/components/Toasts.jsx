import React from 'react';
import { CheckCircle, AlertTriangle, XCircle, Info, X } from 'lucide-react';

const STYLES = {
  success: { border: 'border-profit',     icon: CheckCircle,   iconClass: 'text-profit' },
  warning: { border: 'border-yellow-400', icon: AlertTriangle, iconClass: 'text-yellow-400' },
  error:   { border: 'border-loss',       icon: XCircle,       iconClass: 'text-loss' },
  info:    { border: 'border-accent',     icon: Info,          iconClass: 'text-accent' },
};

export default function Toasts({ toasts, onDismiss }) {
  if (toasts.length === 0) return null;

  return (
    <div className="fixed bottom-10 left-4 z-50 flex flex-col gap-2 pointer-events-none">
      {toasts.map((t) => {
        const style = STYLES[t.type] || STYLES.info;
        const Icon = style.icon;
        return (
          <div
            key={t.id}
            className={`pointer-events-auto animate-slide-in min-w-[260px] max-w-[360px] bg-surface-card border-l-4 ${style.border} border border-surface-border rounded-lg shadow-lg px-3 py-2.5 flex items-start gap-2.5`}
          >
            <Icon size={16} className={`${style.iconClass} shrink-0 mt-0.5`} />
            <span className="text-sm text-gray-200 flex-1">{t.message}</span>
            <button
              onClick={() => onDismiss(t.id)}
              className="text-gray-600 hover:text-white transition-colors shrink-0"
            >
              <X size={14} />
            </button>
          </div>
        );
      })}
    </div>
  );
}
