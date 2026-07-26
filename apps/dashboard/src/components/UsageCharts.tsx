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
  <div className="flex flex-wrap gap-1 border-b border-border-slate pb-2 text-xs" aria-label="Usage time range">
    {(Object.keys(TOKEN_RANGE_LABELS) as TokenRange[]).map(range => (
      <button
        key={range}
        type="button"
        className={`rounded px-3 py-1.5 font-medium transition-colors ${
          value === range
            ? 'bg-queue-blue text-[#0b1017]'
            : 'text-text-muted hover:bg-white/5 hover:text-text-primary'
        }`}
        onClick={() => onChange(range)}
      >
        {TOKEN_RANGE_LABELS[range]}
      </button>
    ))}
  </div>
);

export const UsageLineChart: React.FC<{
  labels: string[];
  series: UsageChartSeries[];
  ariaLabel: string;
  formatValue?: (value: number) => string;
}> = ({ labels, series, ariaLabel, formatValue = value => value.toLocaleString() }) => {
  const width = 680;
  const height = 170;
  const lastIndex = labels.length - 1;
  const max = Math.max(1, ...series.flatMap(item => item.values));
  const svgRef = useRef<SVGSVGElement>(null);
  const [hoverIndex, setHoverIndex] = useState<number | null>(null);
  const x = (index: number) => lastIndex <= 0 ? width / 2 : width / lastIndex * index;
  const y = (value: number) => height - clamp(value, 0, max) / max * height;
  const tickIndices = pickTickIndices(labels.length);
  const edgeTranslateX = (index: number) =>
    index === 0 ? '0%' : index === lastIndex ? '-100%' : '-50%';

  if (!labels.length || !series.some(item => item.values.some(value => value > 0))) {
    return (
      <p className="border-y border-dashed border-border-slate py-8 text-center text-sm text-text-muted">
        No usage recorded for this range.
      </p>
    );
  }

  const handlePointerMove = (event: React.MouseEvent<SVGSVGElement>) => {
    const rect = svgRef.current?.getBoundingClientRect();
    if (!rect) return;
    const ratio = clamp((event.clientX - rect.left) / rect.width, 0, 1);
    setHoverIndex(lastIndex <= 0 ? 0 : Math.round(ratio * lastIndex));
  };

  return (
    <div className="mt-4">
      <div className="grid grid-cols-[42px_1fr] gap-2 text-xs text-text-muted">
        <div className="flex flex-col justify-between py-2">
          <span>{formatValue(max)}</span>
          <span>{formatValue(max / 2)}</span>
          <span>0</span>
        </div>
        <div>
          <div className="relative">
            <svg
              ref={svgRef}
              viewBox={`0 0 ${width} ${height}`}
              preserveAspectRatio="none"
              className="h-[170px] w-full overflow-visible"
              role="img"
              aria-label={ariaLabel}
              onMouseMove={handlePointerMove}
              onMouseLeave={() => setHoverIndex(null)}
            >
              <g stroke="rgba(148,163,184,0.14)" strokeDasharray="4 5" vectorEffect="non-scaling-stroke">
                {[0, height / 2, height].map(gridY => (
                  <line key={gridY} x1="0" y1={gridY} x2={width} y2={gridY} vectorEffect="non-scaling-stroke" />
                ))}
              </g>
              {series.map(item => (
                <path
                  key={item.label}
                  d={linePath(item.values, width, height, max)}
                  fill="none"
                  stroke={item.color}
                  strokeWidth="3"
                  strokeLinecap="round"
                  strokeLinejoin="round"
                  vectorEffect="non-scaling-stroke"
                />
              ))}
              {hoverIndex !== null && (
                <g pointerEvents="none">
                  <line x1={x(hoverIndex)} y1="0" x2={x(hoverIndex)} y2={height} stroke="rgba(226,232,240,0.35)" vectorEffect="non-scaling-stroke" />
                  {series.map(item => (
                    <circle key={item.label} cx={x(hoverIndex)} cy={y(item.values[hoverIndex] ?? 0)} r="3.5" fill={item.color} />
                  ))}
                </g>
              )}
            </svg>
            {hoverIndex !== null && (
              <div
                className="pointer-events-none absolute z-10 whitespace-nowrap rounded-md border border-white/10 bg-[#0b1626] px-2.5 py-1.5 text-xs shadow-deck"
                style={{
                  left: `${x(hoverIndex) / width * 100}%`,
                  top: '0',
                  transform: `translateX(${edgeTranslateX(hoverIndex)})`,
                }}
              >
                <div className="font-medium text-text-primary">{labels[hoverIndex] || 'Period'}</div>
                <div className="mt-1 space-y-0.5 text-text-secondary">
                  {series.map(item => (
                    <div key={item.label}>
                      {item.label}: <span className="text-text-primary">{formatValue(item.values[hoverIndex] ?? 0)}</span>
                    </div>
                  ))}
                </div>
              </div>
            )}
          </div>
          <div className="relative mt-1 h-4 text-xs text-text-muted">
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
      </div>
      <div className="mt-4 flex flex-wrap gap-x-5 gap-y-2 text-xs text-text-secondary">
        {series.map(item => (
          <span key={item.label} className="inline-flex items-center gap-2">
            <span className="h-0.5 w-6" style={{ background: item.color }} />
            {item.label}
          </span>
        ))}
      </div>
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
