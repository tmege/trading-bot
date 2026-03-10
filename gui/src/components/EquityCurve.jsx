import React, { useEffect, useRef } from 'react';

export default function EquityCurve({ data, title = 'Equity Curve' }) {
  const chartRef = useRef(null);
  const chartInstance = useRef(null);

  useEffect(() => {
    if (!data || data.length === 0 || !chartRef.current) return;

    let mounted = true;

    import('lightweight-charts').then(({ createChart }) => {
      if (!mounted || !chartRef.current) return;

      // Clean up previous chart
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
        crosshair: {
          mode: 0,
        },
        rightPriceScale: {
          borderColor: '#30363d',
        },
        timeScale: {
          borderColor: '#30363d',
          timeVisible: true,
        },
      });

      chartInstance.current = chart;

      // Determine if profitable
      const lastEquity = data[data.length - 1]?.equity ?? 100;
      const firstEquity = data[0]?.equity ?? 100;
      const profitable = lastEquity >= firstEquity;

      const areaSeries = chart.addAreaSeries({
        topColor: profitable ? 'rgba(0, 200, 83, 0.3)' : 'rgba(255, 23, 68, 0.3)',
        bottomColor: profitable ? 'rgba(0, 200, 83, 0.02)' : 'rgba(255, 23, 68, 0.02)',
        lineColor: profitable ? '#00c853' : '#ff1744',
        lineWidth: 2,
      });

      const chartData = data.map(pt => ({
        time: Math.floor(pt.time_ms / 1000),
        value: pt.equity,
      }));

      areaSeries.setData(chartData);
      chart.timeScale().fitContent();

      // Handle resize
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
  }, [data]);

  if (!data || data.length === 0) {
    return null;
  }

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg overflow-hidden">
      <div className="px-4 py-2 border-b border-surface-border">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider">{title}</h3>
      </div>
      <div ref={chartRef} />
    </div>
  );
}
