import React from 'react';

interface MetricLine {
  label: string;
  value: React.ReactNode;
  tone?: 'green' | 'violet' | 'blue' | 'amber' | 'rose' | 'mint' | 'muted';
  bar?: number;
}

interface MetricCardProps {
  title: string;
  icon?: React.ReactNode;
  lines: MetricLine[];
}

const tones = {
  green: 'text-success-green',
  violet: 'text-infer-violet',
  blue: 'text-queue-blue',
  amber: 'text-warning-amber',
  rose: 'text-danger-rose',
  mint: 'text-gpu-mint',
  muted: 'text-text-secondary',
};

export const MetricCard: React.FC<MetricCardProps> = ({ title, icon, lines }) => (
  <div className="min-w-0 rounded-xl border border-border-slate bg-panel-slate p-4">
    <div className="mb-4 flex items-center justify-between gap-2">
      <p className="truncate text-xs uppercase tracking-wide text-text-secondary">{title}</p>
      {icon && <div className="flex h-6 w-6 shrink-0 items-center justify-center rounded-md border border-queue-blue/30 bg-queue-blue/10 text-queue-blue">{icon}</div>}
    </div>
    <div className="space-y-3">
      {lines.map((line) => (
        <div key={line.label} className="min-w-0">
          <div className="flex min-w-0 items-center justify-between gap-3 text-sm">
            <span className="truncate text-text-secondary">{line.label}</span>
            <span className={`truncate text-right font-medium ${line.tone ? tones[line.tone] : 'text-text-primary'}`}>{line.value}</span>
          </div>
          {typeof line.bar === 'number' && (
            <div className="mt-1 h-1.5 overflow-hidden rounded-full bg-card-highlight">
              <div className="h-full rounded-full bg-gradient-to-r from-infer-violet to-ion-cyan" style={{ width: `${Math.max(0, Math.min(line.bar, 100))}%` }} />
            </div>
          )}
        </div>
      ))}
    </div>
  </div>
);
