import React from 'react';
import type { Tone } from '../types';
import { clamp, toneBg, toneHex, toneLabel, toneText } from '../utils';
import { SlidingIndicator, useSlidingIndicator } from './motion';

type Children = { children?: React.ReactNode };

export const Panel: React.FC<{ children: React.ReactNode; className?: string; 'aria-label'?: string; tight?: boolean }> = ({ children, className = '', tight, ...rest }) => (
  <section className={`border-t border-border-slate ${tight ? '' : 'pt-4'} ${className}`} {...rest}>{children}</section>
);

export const SectionTitle: React.FC<{ title: string; aside?: string; action?: React.ReactNode }> = ({ title, aside, action }) => (
  <div className="flex min-w-0 flex-wrap items-center justify-between gap-x-3 gap-y-2">
    <h2 className="min-w-0 text-sm font-semibold text-text-primary">
      {title}
      {aside && <span className="ml-2 font-normal text-text-muted">{aside}</span>}
    </h2>
    {action && <div className="shrink-0">{action}</div>}
  </div>
);

export const PageHeader: React.FC<{
  title: string;
  subtitle?: React.ReactNode;
  actions?: React.ReactNode;
  tint?: string;
  icon?: React.ReactNode;
}> = ({ title, subtitle, actions }) => (
  <header className="flex flex-wrap items-end justify-between gap-x-6 gap-y-3">
    <div className="min-w-0">
      <h1 className="text-2xl font-semibold text-text-primary">{title}</h1>
      {subtitle && <p className="mt-1 max-w-[65ch] text-sm text-text-muted">{subtitle}</p>}
    </div>
    {actions && <div className="flex flex-wrap items-center gap-2">{actions}</div>}
  </header>
);

export const Badge: React.FC<{ label: string; tone: Tone }> = ({ label, tone }) => (
  <span className={`inline-flex shrink-0 items-center whitespace-nowrap rounded px-1.5 py-0.5 text-2xs font-medium ${toneBg(tone)} ${toneText(tone)}`}>
    {label}
  </span>
);

export const Dot: React.FC<{ tone: Tone; pulse?: boolean }> = ({ tone }) => (
  <span className="inline-block h-2 w-2 shrink-0 rounded-full" style={{ background: toneHex(tone) }} aria-hidden="true" />
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
        <p className="truncate text-xs text-text-muted">{label}</p>
        {badgeLabel && <Badge label={badgeLabel} tone={tone} />}
      </div>
      <p className={`tabular mt-0.5 truncate font-semibold ${size === 'hero' ? 'text-2xl' : 'text-xl'} ${tone === 'idle' ? 'text-text-primary' : toneText(tone)}`}>{value}</p>
      {sub && <p className="mt-0.5 truncate text-xs text-text-muted">{sub}</p>}
    </div>
  );
};

export const Readout: React.FC<Children & { className?: string; 'aria-label'?: string }> = ({ children, className = '', ...rest }) => (
  <div className={`grid grid-cols-2 overflow-hidden rounded-lg border border-border-slate bg-panel-slate shadow-card sm:auto-cols-fr sm:grid-flow-col sm:grid-cols-none ${className}`} {...rest}>{children}</div>
);

export const StatTile: React.FC<{
  label: string;
  value: string;
  tone?: Tone;
  sub?: string;
  icon?: React.ReactNode;
  tint?: string;
}> = ({ label, value, tone = 'idle', sub }) => (
  <div className="-ml-px -mt-px min-w-0 border-l border-t border-border-slate px-4 py-3">
    <p className="truncate text-xs text-text-muted">{label}</p>
    <p className={`tabular mt-1 truncate text-xl font-semibold ${tone === 'idle' ? 'text-text-primary' : toneText(tone)}`}>{value}</p>
    {sub && <p className="mt-0.5 truncate text-xs text-text-muted">{sub}</p>}
  </div>
);

export const DetailItem: React.FC<{
  label: string;
  children: React.ReactNode;
  className?: string;
}> = ({ label, children, className = '' }) => (
  <div className={`min-w-0 ${className}`}>
    <dt className="text-xs text-text-muted">{label}</dt>
    <dd className="mt-0.5 break-words text-sm text-text-secondary">{children}</dd>
  </div>
);

type ButtonTone = 'blue' | 'green' | 'neutral' | 'danger' | 'plain';

export function buttonPalette(tone: ButtonTone): string {
  if (tone === 'blue') return 'bg-queue-blue font-semibold text-on-accent shadow-card hover:bg-queue-blue/90';
  if (tone === 'green') return 'border border-line-strong bg-panel-slate text-success-green shadow-card hover:bg-elevated-slate';
  if (tone === 'danger') return 'border border-line-strong bg-panel-slate text-danger-rose shadow-card hover:bg-elevated-slate';
  if (tone === 'plain') return 'text-queue-blue hover:bg-queue-blue/10';
  return 'border border-line-strong bg-panel-slate text-text-primary shadow-card hover:bg-elevated-slate';
}

export const buttonBase = 'inline-flex min-h-10 items-center justify-center gap-1.5 whitespace-nowrap rounded-md px-3 text-sm font-medium disabled:cursor-not-allowed disabled:opacity-40 sm:min-h-8';

export const Spinner: React.FC<{ className?: string }> = ({ className = 'h-3.5 w-3.5' }) => (
  <span className={`inline-block shrink-0 animate-spin rounded-full border-2 border-current border-t-transparent ${className}`} aria-hidden="true" />
);

export const Button: React.FC<{
  children: React.ReactNode;
  onClick?: () => void;
  tone?: ButtonTone;
  type?: 'button' | 'submit';
  disabled?: boolean;
  loading?: boolean;
  className?: string;
  title?: string;
}> = ({ children, onClick, tone = 'neutral', type = 'button', disabled, loading, className = '', title }) => (
  <button
    type={type}
    disabled={disabled || loading}
    aria-busy={loading || undefined}
    onClick={onClick}
    title={title}
    className={`${buttonBase} ${buttonPalette(tone)} ${className}`}
  >
    {loading && <Spinner />}
    {children}
  </button>
);

export const IconButton: React.FC<{
  label: string;
  children: React.ReactNode;
  onClick?: () => void;
  tone?: ButtonTone;
  disabled?: boolean;
  className?: string;
}> = ({ label, children, onClick, tone = 'neutral', disabled, className = '' }) => (
  <button
    type="button"
    aria-label={label}
    title={label}
    disabled={disabled}
    onClick={onClick}
    className={`inline-flex h-10 w-10 shrink-0 items-center justify-center rounded-md transition-colors hover:bg-elevated-slate disabled:cursor-not-allowed disabled:opacity-40 sm:h-8 sm:w-8 ${tone === 'danger' ? 'text-danger-rose' : tone === 'blue' ? 'text-queue-blue' : 'text-text-secondary hover:text-text-primary'} ${className}`}
  >
    {children}
  </button>
);

export const Switch: React.FC<{
  checked: boolean;
  onChange: (checked: boolean) => void;
  label: string;
  disabled?: boolean;
  tint?: string;
}> = ({ checked, onChange, label, disabled }) => (
  <button
    type="button"
    role="switch"
    aria-checked={checked}
    aria-label={label}
    disabled={disabled}
    onClick={() => onChange(!checked)}
    className={`relative inline-flex h-6 w-10 shrink-0 items-center rounded-full transition-colors duration-200 disabled:cursor-not-allowed disabled:opacity-40 ${checked ? 'bg-queue-blue' : 'bg-card-highlight'}`}
  >
    <span className={`absolute left-0.5 h-5 w-5 rounded-full bg-white shadow-[0_1px_3px_rgb(0_0_0/0.25)] transition-transform duration-200 ease-[cubic-bezier(0.3,0.9,0.3,1)] ${checked ? 'translate-x-4' : ''}`} />
  </button>
);

export const Segmented: React.FC<{
  items: Array<{ id: string; label: string; count?: number }>;
  value: string;
  onChange: (id: string) => void;
  'aria-label': string;
  className?: string;
}> = ({ items, value, onChange, className = '', ...rest }) => {
  const { container, box, animated, ready } = useSlidingIndicator<HTMLDivElement>(value);
  return (
    <div className={`max-w-full overflow-x-auto ${className}`}>
      <div ref={container} role="tablist" aria-label={rest['aria-label']} className="relative inline-flex gap-0.5 rounded-lg bg-elevated-slate p-0.5">
        <SlidingIndicator box={box} animated={animated} />
        {items.map(item => {
          const active = item.id === value;
          return (
            <button
              key={item.id}
              type="button"
              role="tab"
              aria-selected={active}
              data-active={active}
              onClick={() => onChange(item.id)}
              className={`relative flex min-h-9 shrink-0 items-center gap-1.5 whitespace-nowrap rounded-md px-3 text-sm font-medium sm:min-h-7 ${
                active ? `text-text-primary ${ready ? '' : 'bg-panel-slate shadow-card'}` : 'text-text-secondary hover:text-text-primary'
              }`}
            >
              {item.label}
              {item.count != null && <span className="tabular text-xs text-text-muted">{item.count}</span>}
            </button>
          );
        })}
      </div>
    </div>
  );
};

export const GroupHeader: React.FC<{ title: string; aside?: React.ReactNode }> = ({ title, aside }) => (
  <div className="flex items-end justify-between gap-3 pb-2 pt-6 first:pt-0">
    <h3 className="text-sm font-semibold text-text-primary">{title}</h3>
    {aside && <div className="text-xs text-text-muted">{aside}</div>}
  </div>
);

export const GroupList: React.FC<Children & { className?: string; inset?: number; 'aria-label'?: string }> = ({ children, className = '', inset: _inset, ...rest }) => (
  <div className={`divide-y divide-border-slate overflow-hidden rounded-lg border border-border-slate bg-panel-slate shadow-card ${className}`} {...rest}>
    {children}
  </div>
);

export const Row: React.FC<Children & {
  leading?: React.ReactNode;
  title: React.ReactNode;
  subtitle?: React.ReactNode;
  trailing?: React.ReactNode;
  onClick?: () => void;
  selected?: boolean;
  className?: string;
  as?: 'div' | 'button' | 'label';
}> = ({ leading, title, subtitle, trailing, onClick, selected, className = '', children, as }) => {
  const Tag: React.ElementType = as ?? (onClick ? 'button' : 'div');
  return (
    <Tag
      {...(onClick ? { onClick, type: 'button' } : {})}
      className={`flex min-h-11 w-full items-center gap-3 px-3 py-2 text-left ${onClick ? 'transition-colors hover:bg-panel-slate' : ''} ${selected ? 'bg-elevated-slate' : ''} ${className}`}
    >
      {leading}
      <div className="min-w-0 flex-1">
        <div className="truncate text-sm text-text-primary">{title}</div>
        {subtitle && <div className="mt-0.5 truncate text-xs text-text-muted">{subtitle}</div>}
        {children}
      </div>
      {trailing != null && <div className="flex shrink-0 items-center gap-2 text-sm text-text-muted">{trailing}</div>}
    </Tag>
  );
};

export const ProgressBar: React.FC<{ percent: number; tone?: Tone; indeterminate?: boolean }> = ({ percent, tone = 'good', indeterminate }) => (
  <div className="h-1.5 overflow-hidden rounded-full bg-elevated-slate">
    <div
      className="h-full rounded-full transition-[width] duration-500 ease-out"
      style={{ width: indeterminate ? '33%' : `${clamp(percent, 0, 100)}%`, background: toneHex(tone) }}
    />
  </div>
);

export const Meter: React.FC<{ label: string; value: string; percent: number; tone?: Tone; sub?: string }> = ({ label, value, percent, tone = 'info', sub }) => (
  <div className="min-w-0">
    <div className="flex items-baseline justify-between gap-3">
      <span className="truncate text-sm text-text-secondary">{label}</span>
      <span className="tabular text-sm font-medium text-text-primary">{value}</span>
    </div>
    <div className="mt-1.5"><ProgressBar percent={percent} tone={tone} /></div>
    {sub && <p className="mt-1 truncate text-xs text-text-muted">{sub}</p>}
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
  const height = 40;
  const max = yMax ?? Math.max(1, ...values);
  const path = linePath(values, width, height - 2, max);
  const area = `${path} L ${width} ${height} L 0 ${height} Z`;
  const badgeLabel = statusLabel ? toneLabel(tone) : '';
  const color = tone === 'warn' || tone === 'critical' ? toneHex(tone) : 'rgb(var(--accent))';
  return (
    <div className="flex min-w-0 flex-col px-4 py-3">
      <div className="flex items-baseline justify-between gap-3">
        <p className="truncate text-xs text-text-muted">{label}</p>
        {badgeLabel ? <Badge label={badgeLabel} tone={tone} /> : null}
      </div>
      <p className="tabular mt-1 truncate text-xl font-semibold text-text-primary">{display}</p>
      {sub && <p className="truncate text-xs text-text-muted">{sub}</p>}
      <svg viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none" className="chart-reveal mt-2 h-10 w-full" role="img" aria-label={`${label} sparkline`}>
        <path d={area} style={{ fill: color, fillOpacity: 0.1 }} />
        <path d={path} style={{ fill: 'none', stroke: color }} strokeWidth="1.5" strokeLinejoin="round" vectorEffect="non-scaling-stroke" />
      </svg>
    </div>
  );
};

export const EmptyState: React.FC<{ title: string; detail?: string; icon?: React.ReactNode; action?: React.ReactNode }> = ({ title, detail, action }) => (
  <div className="rounded-lg border border-dashed border-line-strong px-5 py-8 text-center">
    <p className="text-sm font-medium text-text-secondary">{title}</p>
    {detail && <p className="mx-auto mt-1 max-w-sm text-sm text-text-muted">{detail}</p>}
    {action && <div className="mt-4">{action}</div>}
  </div>
);

export const Notice: React.FC<Children & { tone: Tone; role?: 'alert' | 'status'; className?: string }> = ({ tone, children, role, className = '' }) => (
  <div role={role} className={`flex items-start gap-2.5 rounded-lg border border-border-slate bg-panel-slate px-3 py-2.5 text-sm text-text-secondary shadow-card ${className}`} style={{ borderLeftColor: toneHex(tone), borderLeftWidth: 3 }}>
    <div className="min-w-0 flex-1 break-words">{children}</div>
  </div>
);
