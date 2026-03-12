import React, { useEffect } from 'react';

export default function ConfirmModal({ open, title, message, confirmLabel = 'Confirm', confirmVariant = 'default', onConfirm, onCancel }) {
  useEffect(() => {
    if (!open) return;
    function handler(e) {
      if (e.key === 'Escape') onCancel();
    }
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, [open, onCancel]);

  if (!open) return null;

  const btnClass = confirmVariant === 'danger'
    ? 'bg-loss/20 text-loss border-loss/30 hover:bg-loss/30'
    : 'bg-accent/20 text-accent border-accent/30 hover:bg-accent/30';

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60" onClick={onCancel}>
      <div
        className="bg-surface-card border border-surface-border rounded-xl shadow-2xl p-6 max-w-md w-full mx-4 animate-modal-in"
        onClick={(e) => e.stopPropagation()}
      >
        <h3 className="text-lg font-semibold text-white mb-2">{title}</h3>
        <p className="text-sm text-gray-400 mb-6">{message}</p>
        <div className="flex justify-end gap-3">
          <button
            onClick={onCancel}
            className="px-4 py-2 text-sm text-gray-400 border border-surface-border rounded-lg hover:bg-surface-hover transition-colors"
          >
            Cancel
          </button>
          <button
            onClick={onConfirm}
            className={`px-4 py-2 text-sm font-medium border rounded-lg transition-colors ${btnClass}`}
          >
            {confirmLabel}
          </button>
        </div>
      </div>
    </div>
  );
}
