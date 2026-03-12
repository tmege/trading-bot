import React, { useEffect, useRef } from 'react';

const COLORS = ['#58a6ff', '#00c853', '#ff9100'];

export default function ComparisonChart({ comparisons }) {
  const chartRef = useRef(null);
  const chartInstance = useRef(null);

  useEffect(() => {
    if (!comparisons || comparisons.length < 2 || !chartRef.current) return;

    let mounted = true;

    import('lightweight-charts').then(({ createChart }) => {
      if (!mounted || !chartRef.current) return;

      if (chartInstance.current) {
        chartInstance.current.remove();
      }

      const chart = createChart(chartRef.current, {
        width: chartRef.current.clientWidth,
        height: 300,
        layout: {
          background: { color: '#161b22' },
          textColor: '#8b949e',
          fontSize: 11,
        },
        grid: {
          vertLines: { color: '#21262d' },
          horzLines: { color: '#21262d' },
        },
        crosshair: { mode: 0 },
        rightPriceScale: { borderColor: '#30363d' },
        timeScale: { borderColor: '#30363d', timeVisible: true },
      });

      chartInstance.current = chart;

      comparisons.slice(0, 3).forEach((comp, i) => {
        const color = COLORS[i];
        const series = chart.addLineSeries({
          color,
          lineWidth: 2,
          title: comp.label,
        });
        const data = (comp.equityCurve || []).map(pt => ({
          time: Math.floor(pt.time_ms / 1000),
          value: pt.equity,
        }));
        series.setData(data);
      });

      chart.timeScale().fitContent();

      const ro = new ResizeObserver(() => {
        if (chartRef.current) {
          chart.applyOptions({ width: chartRef.current.clientWidth });
        }
      });
      ro.observe(chartRef.current);
    });

    return () => {
      mounted = false;
      if (chartInstance.current) {
        chartInstance.current.remove();
        chartInstance.current = null;
      }
    };
  }, [comparisons]);

  if (!comparisons || comparisons.length < 2) return null;

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg overflow-hidden">
      <div className="px-4 py-2 border-b border-surface-border flex items-center justify-between">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider">Strategy Comparison</h3>
        <div className="flex items-center gap-3">
          {comparisons.slice(0, 3).map((comp, i) => (
            <div key={i} className="flex items-center gap-1.5">
              <span className="w-3 h-0.5 rounded" style={{ backgroundColor: COLORS[i] }} />
              <span className="text-[10px] text-gray-400">{comp.label}</span>
            </div>
          ))}
        </div>
      </div>
      <div ref={chartRef} />
    </div>
  );
}
