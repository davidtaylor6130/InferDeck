import React from 'react';
import { StatusBadge } from './StatusBadge';

interface ResourceLockCardProps {
  locked: boolean;
  owner?: string | null;
}

export const ResourceLockCard: React.FC<ResourceLockCardProps> = ({ locked, owner }) => (
  <div className="rounded-xl border border-border-slate bg-panel-slate p-4">
    <div className="flex items-center justify-between gap-3">
      <div className="min-w-0">
        <p className="text-xs uppercase tracking-wide text-text-secondary">GPU Lock</p>
        <p className={`mt-2 text-2xl font-bold ${locked ? 'text-queue-blue' : 'text-gpu-mint'}`}>{locked ? 'Locked' : 'Free'}</p>
      </div>
      <StatusBadge label={locked ? 'Locked' : 'Free'} tone={locked ? 'locked' : 'free'} />
    </div>
    <p className="mt-3 truncate text-sm text-text-secondary" title={owner || ''}>{locked ? `Owner: ${owner || 'active workload'}` : 'No active workload'}</p>
  </div>
);
