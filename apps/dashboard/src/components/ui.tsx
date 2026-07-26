import React from 'react';
import type { Tone } from '../types';
import { clamp, toneBg, toneHex, toneLabel, toneText } from '../utils';

export const Panel: React.FC<{ children: React.ReactNode; className?: string }> = ({ children, className = '' }) => (
  <section className={`border-t border-border-slate pt-4 ${className}`}>{children}</section>
);

export const SectionTitle: React.FC<{ title: string; aside?: string; action?: React.ReactNode }> = ({ title, aside, action }) => (
  <div className="flex min-w-0 items-center justify-between gap-3">
    <h2 className="truncate text-base font-semibold text-text-primary">
      {title} {aside && <span className="text-xs font-normal text-text-muted">({aside})</span>}
    </h2>
    {action}
  </div>
);

export const Badge: React.FC<{ label: string; tone: Tone }> = ({ label, tone }) => (
  <span className={`inline-flex items-center rounded px-2 py-0.5 text-xs font-medium ${toneBg(tone)} ${toneText(tone)}`}>
    {label}
  </span>
);

export const Stat: React.FC<{
  label: string;
  value: string;
  tone?: Tone;
  sub?: string;
  statusLabel?: boolean;
  size?: 'hero';
}> = ({ label, value, tone = 'idle', sub, statusLabel, size }) => {
  const badgeLabel = statusLabel ? toneLabel(tone) : '';
  return (
    <div className="min-w-0">
      <div className="flex items-center gap-2">
        <p className={`truncate text-text-muted ${size === 'hero' ? 'text-sm' : 'text-xs'}`}>{label}</p>
        {badgeLabel && <Badge label={badgeLabel} tone={tone} />}
      </div>
      <p className={`mt-0.5 truncate font-semibold ${size === 'hero' ? 'text-2xl' : 'text-lg'} ${tone === 'idle' ? 'text-text-primary' : toneText(tone)}`}>{value}</p>
      {sub && <p className="truncate text-xs text-text-muted">{sub}</p>}
    </div>
  );
};

export const Button: React.FC<{
  children: React.ReactNode;
  onClick?: () => void;
  tone?: 'blue' | 'green' | 'neutral' | 'danger';
  disabled?: boolean;
  className?: string;
}> = ({ children, onClick, tone = 'neutral', disabled, className = '' }) => {
  const palette = tone === 'blue'
    ? 'border-queue-blue/40 bg-queue-blue/15 text-queue-blue hover:bg-queue-blue/25'
    : tone === 'green'
      ? 'border-success-green/40 bg-success-green/15 text-success-green hover:bg-success-green/25'
    : tone === 'danger'
      ? 'border-danger-rose/40 bg-danger-rose/15 text-danger-rose hover:bg-danger-rose/25'
      : 'border-white/15 bg-white/[0.06] text-text-primary hover:bg-white/[0.12]';
  return (
    <button
      type="button"
      disabled={disabled}
      onClick={onClick}
      className={`inline-flex min-h-9 items-center justify-center rounded border px-3 py-1.5 text-xs font-medium transition-colors focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-queue-blue disabled:cursor-not-allowed disabled:opacity-40 ${palette} ${className}`}
    >
      {children}
    </button>
  );
};

export const ProgressBar: React.FC<{ percent: number; tone?: Tone; indeterminate?: boolean }> = ({ percent, tone = 'good', indeterminate }) => (
  <div className="h-1.5 overflow-hidden bg-white/10">
    {indeterminate ? (
      <div className="h-full w-1/3 animate-pulse" style={{ background: toneHex(tone) }} />
    ) : (
      <div className="h-full transition-[width]" style={{ width: `${clamp(percent, 0, 100)}%`, background: toneHex(tone) }} />
    )}
  </div>
);

export function linePath(values: number[], width: number, height: number, max: number): string {
  if (!values.length) return `M 0 ${height}`;
  if (values.length === 1) {
    const y = height - (clamp(values[0], 0, max) / max) * height;
    return `M 0 ${y.toFixed(2)} L ${width} ${y.toFixed(2)}`;
  }
  return values.map((value, index) => {
    const x = (width / (values.length - 1)) * index;
    const y = height - (clamp(value, 0, max) / max) * height;
    return `${index === 0 ? 'M' : 'L'} ${x.toFixed(2)} ${y.toFixed(2)}`;
  }).join(' ');
}

// Picks up to maxTicks bucket indices, evenly spaced by index, always including the first and last.
// Kept separate from label text so axis-tick thinning never drifts from the plotted point positions.
export function pickTickIndices(count: number, maxTicks = 8): number[] {
  if (count <= 0) return [];
  if (count <= maxTicks) return Array.from({ length: count }, (_, i) => i);
  const indices = new Set<number>();
  for (let i = 0; i < maxTicks; i++) indices.add(Math.round((i * (count - 1)) / (maxTicks - 1)));
  return Array.from(indices).sort((a, b) => a - b);
}

export const Sparkline: React.FC<{
  label: string;
  display: string;
  values: number[];
  tone: Tone;
  yMax?: number;
  sub?: string;
  statusLabel?: boolean;
}> = ({ label, display, values, tone, yMax, sub, statusLabel }) => {
  const width = 220;
  const height = 64;
  const max = yMax ?? Math.max(1, ...values);
  const path = linePath(values, width, height, max);
  const area = `${path} L ${width} ${height} L 0 ${height} Z`;
  const badgeLabel = statusLabel ? toneLabel(tone) : '';
  return (
    <div className="border-l border-border-slate pl-3">
      <div className="flex items-start justify-between gap-3">
        <div className="min-w-0">
          <p className="truncate text-xs font-medium text-text-secondary">{label}</p>
          <p className="mt-0.5 truncate text-lg font-semibold text-text-primary">{display}</p>
          {sub && <p className="truncate text-xs text-text-muted">{sub}</p>}
        </div>
        {badgeLabel ? <Badge label={badgeLabel} tone={tone} /> : null}
      </div>
      <svg viewBox={`0 0 ${width} ${height}`} className="mt-2 h-16 w-full" role="img" aria-label={`${label} sparkline`}>
        <path d={area} fill={toneHex(tone)} opacity="0.1" />
        <path d={path} fill="none" stroke={toneHex(tone)} strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round" />
      </svg>
    </div>
  );
};

export const EmptyState: React.FC<{ title: string; detail?: string }> = ({ title, detail }) => (
  <div className="border-y border-dashed border-white/15 py-8 text-center">
    <p className="text-sm font-medium text-text-secondary">{title}</p>
    {detail && <p className="mt-1 text-xs text-text-muted">{detail}</p>}
  </div>
);
