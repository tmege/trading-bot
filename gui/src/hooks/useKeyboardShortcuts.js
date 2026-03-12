import { useEffect } from 'react';

const PAGE_KEYS = ['1', '2', '3', '4', '5'];
const PAGES = ['dashboard', 'market', 'strategies', 'backtest', 'settings'];

export default function useKeyboardShortcuts({ onNavigate, onSave, onCloseModal }) {
  useEffect(() => {
    function handler(e) {
      // Ignore when typing in inputs
      const tag = e.target.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;

      const mod = e.metaKey || e.ctrlKey;

      // Cmd/Ctrl + 1-5: navigation
      if (mod && PAGE_KEYS.includes(e.key)) {
        e.preventDefault();
        const idx = parseInt(e.key) - 1;
        if (onNavigate && PAGES[idx]) onNavigate(PAGES[idx]);
        return;
      }

      // Cmd/Ctrl + S: save
      if (mod && e.key === 's') {
        e.preventDefault();
        if (onSave) onSave();
        return;
      }

      // Escape: close modal
      if (e.key === 'Escape') {
        if (onCloseModal) onCloseModal();
      }
    }

    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, [onNavigate, onSave, onCloseModal]);
}
