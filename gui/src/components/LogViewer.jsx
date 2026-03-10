import React, { useEffect, useRef } from 'react';

export default function LogViewer() {
  const termRef = useRef(null);
  const xtermRef = useRef(null);
  const fitAddonRef = useRef(null);

  useEffect(() => {
    let cleanup1, cleanup2;
    let mounted = true;

    async function init() {
      // Dynamic import for xterm (ESM in renderer)
      const { Terminal } = await import('@xterm/xterm');
      const { FitAddon } = await import('@xterm/addon-fit');

      if (!mounted || !termRef.current) return;

      const fitAddon = new FitAddon();
      const term = new Terminal({
        theme: {
          background: '#0d1117',
          foreground: '#e6edf3',
          cursor: '#58a6ff',
          selectionBackground: '#264f78',
        },
        fontSize: 12,
        fontFamily: "'SF Mono', 'Fira Code', 'Cascadia Code', monospace",
        scrollback: 5000,
        convertEol: true,
      });

      term.loadAddon(fitAddon);
      term.open(termRef.current);
      fitAddon.fit();

      xtermRef.current = term;
      fitAddonRef.current = fitAddon;

      // Load initial logs
      try {
        const res = await window.api.logs.tail(200);
        if (res.ok) term.write(res.content);
      } catch (_) {
        term.write('\r\n[Logs unavailable - bot may not be running]\r\n');
      }

      // Stream new lines
      try {
        cleanup1 = window.api.logs.onLine((data) => {
          term.write(data);
        });
      } catch (_) {}

      // Stream bot stdout
      try {
        cleanup2 = window.api.bot.onLog((data) => {
          term.write(data);
        });
      } catch (_) {}

      // Handle resize
      const ro = new ResizeObserver(() => {
        try { fitAddon.fit(); } catch (_) {}
      });
      ro.observe(termRef.current);
    }

    init();

    return () => {
      mounted = false;
      cleanup1?.();
      cleanup2?.();
      xtermRef.current?.dispose();
    };
  }, []);

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg overflow-hidden flex flex-col">
      <div className="px-4 py-2 border-b border-surface-border flex items-center justify-between">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider">Logs</h3>
      </div>
      <div ref={termRef} className="flex-1 min-h-0" style={{ minHeight: '200px' }} />
    </div>
  );
}
