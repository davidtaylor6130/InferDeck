import React from 'react';
import type { ServiceRecord } from '../types';
import { formatDate, getServiceName } from '../utils';
import { CommandButton } from './CommandButton';
import { CopyButton } from './CopyButton';
import { StatusBadge } from './StatusBadge';

interface ServiceStatusCardProps {
  service: ServiceRecord;
  onStart?: (id: string) => void;
  onStop?: (id: string) => void;
  onRestart?: (id: string) => void;
  onCopied?: (message: string) => void;
}

export const ServiceStatusCard: React.FC<ServiceStatusCardProps> = ({ service, onStart, onStop, onRestart, onCopied }) => (
  <div className="rounded-xl border border-border-slate bg-panel-slate p-4">
    <div className="mb-4 flex min-w-0 items-start justify-between gap-3">
      <div className="min-w-0">
        <h3 className="truncate text-lg font-semibold text-text-primary">{getServiceName(service)}</h3>
        <p className="truncate font-mono text-xs text-text-muted">{service.kind}</p>
      </div>
      <StatusBadge label={service.status} tone={service.status} dot />
    </div>
    <dl className="space-y-3 text-sm">
      <div className="flex min-w-0 justify-between gap-3">
        <dt className="text-text-secondary">PID</dt>
        <dd className="truncate font-mono text-text-primary">{service.pid ?? 'N/A'}</dd>
      </div>
      <div className="flex min-w-0 items-center justify-between gap-3">
        <dt className="text-text-secondary">Base URL</dt>
        <dd className="flex min-w-0 items-center gap-2">
          <span className="truncate font-mono text-xs text-text-primary" title={service.baseUrl || 'N/A'}>{service.baseUrl || 'N/A'}</span>
          {service.baseUrl && <CopyButton value={service.baseUrl} label="Service URL" onCopied={onCopied} />}
        </dd>
      </div>
      <div className="flex min-w-0 justify-between gap-3">
        <dt className="text-text-secondary">Last check</dt>
        <dd className="truncate text-text-primary">{formatDate(service.lastHealthcheckAt)}</dd>
      </div>
      <div>
        <dt className="text-text-secondary">Last error</dt>
        <dd className="mt-1 truncate text-danger-rose" title={service.lastError || ''}>{service.lastError || 'None'}</dd>
      </div>
    </dl>
    <div className="mt-4 flex flex-wrap gap-2">
      <CommandButton tone="green" className="min-h-8 px-3 py-1 text-xs" disabled={service.status === 'not_configured'} onClick={() => onStart?.(service.id)}>Start</CommandButton>
      <CommandButton tone="amber" className="min-h-8 px-3 py-1 text-xs" disabled={service.status === 'not_configured'} onClick={() => onStop?.(service.id)}>Stop</CommandButton>
      <CommandButton tone="blue" className="min-h-8 px-3 py-1 text-xs" disabled={service.status === 'not_configured'} onClick={() => onRestart?.(service.id)}>Restart</CommandButton>
    </div>
  </div>
);
