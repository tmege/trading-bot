import React, { useEffect, useRef } from 'react';

export default function CodeViewer({ code, filename }) {
  const codeRef = useRef(null);

  useEffect(() => {
    if (!code || !codeRef.current) return;
    let mounted = true;

    // Highlight with Prism if available (safe: textContent + highlightElement, no innerHTML)
    import('prismjs').then((Prism) => {
      import('prismjs/components/prism-lua').then(() => {
        if (mounted && codeRef.current) {
          codeRef.current.textContent = code;
          Prism.default.highlightElement(codeRef.current);
        }
      });
    }).catch(() => {
      if (mounted && codeRef.current) {
        codeRef.current.textContent = code;
      }
    });

    return () => { mounted = false; };
  }, [code]);

  if (!code) {
    return (
      <div className="flex-1 flex items-center justify-center bg-surface-card border border-surface-border rounded-lg">
        <span className="text-gray-600 text-sm">Select a strategy to view its code</span>
      </div>
    );
  }

  const lines = code.split('\n');

  return (
    <div className="flex-1 bg-surface-card border border-surface-border rounded-lg overflow-hidden flex flex-col">
      <div className="px-4 py-2 border-b border-surface-border flex items-center justify-between">
        <span className="text-xs text-gray-400 font-mono">{filename}</span>
        <span className="text-xs text-gray-600">{lines.length} lines</span>
      </div>
      <div className="flex-1 overflow-auto">
        <div className="flex text-sm font-mono leading-6">
          <div className="text-right pr-4 pl-4 py-3 select-none text-gray-600 bg-surface-bg/50 border-r border-surface-border">
            {lines.map((_, i) => (
              <div key={i}>{i + 1}</div>
            ))}
          </div>
          <pre className="flex-1 py-3 px-4 overflow-x-auto">
            <code ref={codeRef} className="language-lua text-gray-300">
              {code}
            </code>
          </pre>
        </div>
      </div>
    </div>
  );
}
