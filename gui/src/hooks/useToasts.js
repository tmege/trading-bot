import { useState, useCallback, useRef } from 'react';

const DURATIONS = {
  success: 3000,
  warning: 5000,
  error: 8000,
  info: 4000,
};

const MAX_TOASTS = 6;

let nextId = 1;

export default function useToasts() {
  const [toasts, setToasts] = useState([]);
  const timers = useRef({});

  const dismissToast = useCallback((id) => {
    clearTimeout(timers.current[id]);
    delete timers.current[id];
    setToasts((prev) => prev.filter((t) => t.id !== id));
  }, []);

  const addToast = useCallback((type, message, duration) => {
    const id = nextId++;
    const ms = duration || DURATIONS[type] || 4000;

    setToasts((prev) => {
      const next = [{ id, type, message }, ...prev];
      // Remove excess toasts
      if (next.length > MAX_TOASTS) {
        const removed = next.slice(MAX_TOASTS);
        for (const t of removed) {
          clearTimeout(timers.current[t.id]);
          delete timers.current[t.id];
        }
        return next.slice(0, MAX_TOASTS);
      }
      return next;
    });

    timers.current[id] = setTimeout(() => {
      delete timers.current[id];
      setToasts((prev) => prev.filter((t) => t.id !== id));
    }, ms);

    return id;
  }, []);

  return { toasts, addToast, dismissToast };
}
