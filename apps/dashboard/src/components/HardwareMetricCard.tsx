import React from 'react';

interface HardwareMetricCardProps {
  label: string;
  value: string;
  detail?: string;
  percent?: number;
  tone?: 'violet' | 'cyan' | 'mint' | 'amber' | 'rose';
}

const fills = {
  violet: 'from-infer-violet to-queue-blue',
  cyan: 'from-ion-cyan to-queue-blue',
  mint: 'from-gpu-mint to-ion-cyan',
  amber: 'from-warning-amber to-gaming-orange',
  rose: 'from-danger-rose to-warning-amber',
};

export const HardwareMetricCard: React.FC<HardwareMetricCardProps> = ({ label, value, detail, percent, tone = 'cyan' }) => (
  <div className="rounded-xl border border-border-slate bg-panel-slate p-4">
    <p className="truncate text-xs uppercase tracking-wide text-text-secondary">{label}</p>
    <p className="mt-3 truncate text-2xl font-bold text-text-primary">{value}</p>
    {detail && <p className="mt-1 truncate text-sm text-text-secondary">{detail}</p>}
    {typeof percent === 'number' && (
      <div className="mt-4 h-2 overflow-hidden rounded-full bg-card-highlight">
        <div className={`h-full rounded-full bg-gradient-to-r ${fills[tone]}`} style={{ width: `${Math.max(0, Math.min(percent, 100))}%` }} />
      </div>
    )}
  </div>
);
