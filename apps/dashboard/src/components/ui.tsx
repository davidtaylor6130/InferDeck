import React from 'react';
import type { Tone } from '../types';
import { clamp, toneBg, toneHex, toneText } from '../utils';

export const Panel: React.FC<{ children: React.ReactNode; className?: string }> = ({ children, className = '' }) => (
  <div className={`rounded-lg border border-white/10 bg-[#0b1626]/88 p-4 shadow-deck ${className}`}>{children}</div>
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
  <span className={`inline-flex items-center gap-1.5 rounded-md px-2 py-0.5 text-xs font-semibold ${toneBg(tone)} ${toneText(tone)}`}>
    <span className="h-1.5 w-1.5 rounded-full" style={{ background: toneHex(tone) }} />
    {label}
  </span>
);

export const Stat: React.FC<{ label: string; value: string; tone?: Tone; sub?: string }> = ({ label, value, tone = 'idle', sub }) => (
  <div className="min-w-0">
    <p className="truncate text-xs text-text-muted">{label}</p>
    <p className={`mt-0.5 truncate text-lg font-semibold ${tone === 'idle' ? 'text-text-primary' : toneText(tone)}`}>{value}</p>
    {sub && <p className="truncate text-xs text-text-muted">{sub}</p>}
  </div>
);

export const Button: React.FC<{
  children: React.ReactNode;
  onClick?: () => void;
  tone?: 'blue' | 'neutral' | 'danger';
  disabled?: boolean;
  className?: string;
}> = ({ children, onClick, tone = 'neutral', disabled, className = '' }) => {
  const palette = tone === 'blue'
    ? 'border-queue-blue/40 bg-queue-blue/15 text-queue-blue hover:bg-queue-blue/25'
    : tone === 'danger'
      ? 'border-danger-rose/40 bg-danger-rose/15 text-danger-rose hover:bg-danger-rose/25'
      : 'border-white/15 bg-white/[0.06] text-text-primary hover:bg-white/[0.12]';
  return (
    <button
      type="button"
      disabled={disabled}
      onClick={onClick}
      className={`inline-flex min-h-9 items-center justify-center gap-1.5 rounded-md border px-3 py-1.5 text-xs font-medium transition disabled:cursor-not-allowed disabled:opacity-40 ${palette} ${className}`}
    >
      {children}
    </button>
  );
};

export const ProgressBar: React.FC<{ percent: number; tone?: Tone; indeterminate?: boolean }> = ({ percent, tone = 'good', indeterminate }) => (
  <div className="h-2 overflow-hidden rounded-full bg-white/10">
    {indeterminate ? (
      <div className="h-full w-1/3 animate-pulse rounded-full" style={{ background: toneHex(tone) }} />
    ) : (
      <div className="h-full rounded-full transition-all" style={{ width: `${clamp(percent, 0, 100)}%`, background: toneHex(tone) }} />
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

export const Sparkline: React.FC<{
  label: string;
  display: string;
  values: number[];
  tone: Tone;
  yMax?: number;
}> = ({ label, display, values, tone, yMax }) => {
  const width = 220;
  const height = 64;
  const max = yMax ?? Math.max(1, ...values);
  const path = linePath(values, width, height, max);
  const area = `${path} L ${width} ${height} L 0 ${height} Z`;
  return (
    <div className="rounded-lg border border-white/10 bg-[#07101d] p-3">
      <div className="flex items-start justify-between gap-3">
        <div className="min-w-0">
          <p className="truncate text-xs font-medium text-text-secondary">{label}</p>
          <p className="mt-0.5 truncate text-lg font-semibold text-text-primary">{display}</p>
        </div>
        <span className="mt-1 h-2 w-2 shrink-0 rounded-full" style={{ background: toneHex(tone) }} />
      </div>
      <svg viewBox={`0 0 ${width} ${height}`} className="mt-2 h-16 w-full" role="img" aria-label={`${label} sparkline`}>
        <path d={area} fill={toneHex(tone)} opacity="0.1" />
        <path d={path} fill="none" stroke={toneHex(tone)} strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round" />
      </svg>
    </div>
  );
};

export const EmptyState: React.FC<{ title: string; detail?: string }> = ({ title, detail }) => (
  <div className="rounded-lg border border-dashed border-white/15 p-8 text-center">
    <p className="text-sm font-medium text-text-secondary">{title}</p>
    {detail && <p className="mt-1 text-xs text-text-muted">{detail}</p>}
  </div>
);
