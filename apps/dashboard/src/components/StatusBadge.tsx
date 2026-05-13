import React from 'react';

const toneClasses: Record<string, string> = {
  healthy: 'border-success-green/30 bg-success-green/10 text-success-green',
  online: 'border-success-green/30 bg-success-green/10 text-success-green',
  running: 'border-success-green/30 bg-success-green/10 text-success-green',
  ready: 'border-success-green/30 bg-success-green/10 text-success-green',
  free: 'border-gpu-mint/30 bg-gpu-mint/10 text-gpu-mint',
  queued: 'border-infer-violet/30 bg-infer-violet/10 text-infer-violet',
  paused: 'border-warning-amber/30 bg-warning-amber/10 text-warning-amber',
  starting: 'border-warning-amber/30 bg-warning-amber/10 text-warning-amber',
  degraded: 'border-warning-amber/30 bg-warning-amber/10 text-warning-amber',
  maintenance: 'border-warning-amber/30 bg-warning-amber/10 text-warning-amber',
  gaming: 'border-gaming-orange/30 bg-gaming-orange/10 text-gaming-orange',
  locked: 'border-queue-blue/30 bg-queue-blue/10 text-queue-blue',
  failed: 'border-danger-rose/30 bg-danger-rose/10 text-danger-rose',
  error: 'border-danger-rose/30 bg-danger-rose/10 text-danger-rose',
  offline: 'border-danger-rose/30 bg-danger-rose/10 text-danger-rose',
  stopped: 'border-warning-amber/30 bg-warning-amber/10 text-warning-amber',
  not_configured: 'border-border-slate bg-card-highlight text-text-secondary',
};

interface StatusBadgeProps {
  label: string;
  tone?: string;
  dot?: boolean;
  className?: string;
}

export const StatusBadge: React.FC<StatusBadgeProps> = ({ label, tone, dot = false, className = '' }) => {
  const key = (tone || label).toLowerCase();
  const classes = toneClasses[key] || 'border-border-slate bg-card-highlight text-text-secondary';
  return (
    <span className={`inline-flex max-w-full items-center gap-1.5 rounded-full border px-2 py-0.5 text-xs font-medium ${classes} ${className}`}>
      {dot && <span className="h-1.5 w-1.5 shrink-0 rounded-full bg-current" />}
      <span className="truncate">{label}</span>
    </span>
  );
};
