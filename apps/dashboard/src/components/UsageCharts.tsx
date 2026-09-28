import React, { useRef, useState } from 'react';
import { TOKEN_RANGE_LABELS, type TokenRange } from '../cost';
import { clamp } from '../utils';
import { linePath, pickTickIndices } from './ui';

export interface UsageChartSeries {
  label: string;
  color: string;
  values: number[];
}

export const UsageRangeTabs: React.FC<{
  value: TokenRange;
  onChange: (range: TokenRange) => void;
}> = ({ value, onChange }) => (
  <div className="inline-flex max-w-full gap-0.5 overflow-x-auto rounded-lg bg-elevated-slate p-0.5" role="group" aria-label="Usage time range">
    {(Object.keys(TOKEN_RANGE_LABELS) as TokenRange[]).map(range => (
      <button
        key={range}
        type="button"
        aria-pressed={value === range}
        className={`min-h-8 shrink-0 whitespace-nowrap rounded-md px-2.5 text-xs font-medium sm:min-h-6 ${
          value === range
            ? 'bg-panel-slate text-text-primary shadow-card'
            : 'text-text-secondary hover:text-text-primary'
        }`}
        onClick={() => onChange(range)}
      >
        {TOKEN_RANGE_LABELS[range]}
      </button>
    ))}
  </div>
);

export const UsageHeader: React.FC<{
  label: string;
  headline: React.ReactNode;
  detail: React.ReactNode;
  range: TokenRange;
  onRange: (range: TokenRange) => void;
}> = ({ label, headline, detail, range, onRange }) => (
  <header>
    <div className="flex flex-wrap items-center justify-between gap-3">
      <p className="text-sm text-text-muted">{label}</p>
      <UsageRangeTabs value={range} onChange={onRange} />
    </div>
    <h1 className="mt-2 text-2xl font-semibold text-text-primary sm:text-[28px] sm:leading-9">{headline}</h1>
    <p className="mt-1 max-w-[70ch] text-sm text-text-secondary">{detail}</p>
  </header>
);

export const USAGE_RANGE_PHRASE: Record<TokenRange, string> = {
  day: 'in the last 24 hours',
  week: 'in the last week',
  month: 'in the last month',
  year: 'in the last year',
  all: 'in total',
};

export const UsageLineChart: React.FC<{
  labels: string[];
  series: UsageChartSeries[];
  ariaLabel: string;
  formatValue?: (value: number) => string;
  height?: number;
}> = ({ labels, series, ariaLabel, formatValue = value => Math.round(value).toLocaleString(), height = 180 }) => {
  const width = 680;
  const lastIndex = labels.length - 1;
  const max = Math.max(1, ...series.flatMap(item => item.values));
  const svgRef = useRef<SVGSVGElement>(null);
  const [hoverIndex, setHoverIndex] = useState<number | null>(null);
  const x = (index: number) => lastIndex <= 0 ? width / 2 : width / lastIndex * index;
  const y = (value: number) => height - clamp(value, 0, max) / max * height;
  const tickIndices = pickTickIndices(labels.length, 5);
  const edgeTranslateX = (index: number) =>
    index === 0 ? '0%' : index === lastIndex ? '-100%' : '-50%';

  if (!labels.length || !series.some(item => item.values.some(value => value > 0))) {
    return (
      <p className="mt-4 rounded-md border border-dashed border-border-slate py-10 text-center text-sm text-text-muted">
        No usage recorded for this range.
      </p>
    );
  }

  const handlePointerMove = (event: React.PointerEvent<SVGSVGElement>) => {
    const rect = svgRef.current?.getBoundingClientRect();
    if (!rect) return;
    const ratio = clamp((event.clientX - rect.left) / rect.width, 0, 1);
    setHoverIndex(lastIndex <= 0 ? 0 : Math.round(ratio * lastIndex));
  };

  return (
    <div className="mt-4">
      <div className="grid grid-cols-[1fr_44px] gap-2 text-2xs text-text-muted">
        <div className="min-w-0">
          <div className="relative">
            <svg
              ref={svgRef}
              viewBox={`0 0 ${width} ${height}`}
              preserveAspectRatio="none"
              className="w-full touch-pan-y overflow-visible"
              style={{ height }}
              role="img"
              aria-label={ariaLabel}
              onPointerDown={handlePointerMove}
              onPointerMove={handlePointerMove}
              onPointerLeave={() => setHoverIndex(null)}
            >
              <g style={{ stroke: 'rgb(var(--separator))' }}>
                {[0, height / 2, height].map(gridY => (
                  <line key={gridY} x1="0" y1={gridY} x2={width} y2={gridY} vectorEffect="non-scaling-stroke" />
                ))}
              </g>
              {series.map((item, index) => {
                const path = linePath(item.values, width, height, max);
                return (
                  <g key={item.label}>
                    {index === 0 && series.length === 1 && <path d={`${path} L ${width} ${height} L 0 ${height} Z`} fill={item.color} fillOpacity="0.08" />}
                    <path
                      d={path}
                      fill="none"
                      style={{ stroke: item.color }}
                      strokeWidth="2"
                      strokeLinejoin="round"
                      vectorEffect="non-scaling-stroke"
                    />
                  </g>
                );
              })}
              {hoverIndex !== null && (
                <g pointerEvents="none">
                  <line x1={x(hoverIndex)} y1="0" x2={x(hoverIndex)} y2={height} style={{ stroke: 'rgb(var(--line))' }} vectorEffect="non-scaling-stroke" />
                </g>
              )}
            </svg>
            {hoverIndex !== null && series.map(item => (
              <span
                key={item.label}
                className="pointer-events-none absolute h-2.5 w-2.5 -translate-x-1/2 -translate-y-1/2 rounded-full ring-2 ring-black"
                style={{ left: `${x(hoverIndex) / width * 100}%`, top: y(item.values[hoverIndex] ?? 0), background: item.color }}
              />
            ))}
            {hoverIndex !== null && (
              <div
                className="pointer-events-none absolute z-10 max-w-[min(18rem,calc(100vw-2rem))] rounded-md border border-line-strong bg-panel-slate px-2.5 py-1.5 text-xs shadow-deck"
                style={{
                  left: `${x(hoverIndex) / width * 100}%`,
                  top: '-8px',
                  transform: `translate(${edgeTranslateX(hoverIndex)}, -100%)`,
                }}
              >
                <div className="font-semibold text-text-primary">{labels[hoverIndex] || 'Period'}</div>
                <div className="mt-1 space-y-0.5 text-text-secondary">
                  {series.map(item => (
                    <div key={item.label} className="flex items-center gap-2">
                      <span className="h-2 w-2 rounded-full" style={{ background: item.color }} />
                      {item.label}
                      <span className="tabular ml-auto pl-3 font-semibold text-text-primary">{formatValue(item.values[hoverIndex] ?? 0)}</span>
                    </div>
                  ))}
                </div>
              </div>
            )}
          </div>
          <div className="chart-ticks relative mt-1.5 h-4">
            {labels.map((label, index) => tickIndices.includes(index) && (
              <span
                key={`${label}:${index}`}
                className="absolute whitespace-nowrap"
                style={{
                  left: `${lastIndex <= 0 ? 50 : index / lastIndex * 100}%`,
                  transform: `translateX(${edgeTranslateX(index)})`,
                }}
              >
                {label}
              </span>
            ))}
          </div>
        </div>
        <div className="tabular flex flex-col justify-between pb-6 text-right" style={{ height: height + 22 }}>
          <span>{formatValue(max)}</span>
          <span>{formatValue(max / 2)}</span>
          <span>0</span>
        </div>
      </div>
      {series.length > 1 && (
        <div className="mt-3 flex flex-wrap gap-x-4 gap-y-1.5 text-xs text-text-secondary">
          {series.map(item => (
            <span key={item.label} className="inline-flex items-center gap-1.5">
              <span className="h-2 w-2 rounded-full" style={{ background: item.color }} />
              {item.label}
            </span>
          ))}
        </div>
      )}
      <table className="sr-only">
        <caption>{ariaLabel}</caption>
        <thead>
          <tr><th>Period</th>{series.map(item => <th key={item.label}>{item.label}</th>)}</tr>
        </thead>
        <tbody>
          {labels.map((label, index) => (
            <tr key={`${label}:${index}`}>
              <th>{label}</th>
              {series.map(item => <td key={item.label}>{item.values[index] ?? 0}</td>)}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
};
