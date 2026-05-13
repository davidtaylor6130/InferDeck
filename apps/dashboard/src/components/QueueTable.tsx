import React from 'react';
import type { JobRecord } from '../types';
import { formatDate } from '../utils';
import { CommandButton } from './CommandButton';
import { CopyButton } from './CopyButton';
import { StatusBadge } from './StatusBadge';

interface QueueTableProps {
  jobs: JobRecord[];
  onCancel: (id: string) => void;
  onRetry?: (id: string) => void;
  onSelect?: (job: JobRecord) => void;
  onCopied?: (message: string) => void;
}

export const QueueTable: React.FC<QueueTableProps> = ({ jobs, onCancel, onRetry, onSelect, onCopied }) => (
  <div className="overflow-x-auto">
    <table className="min-w-[940px] w-full table-fixed text-left text-sm">
      <thead>
        <tr className="border-b border-border-slate bg-card-highlight/60 text-xs text-text-secondary">
          <th className="w-56 px-3 py-3 font-medium">Job ID</th>
          <th className="w-36 px-3 py-3 font-medium">Type</th>
          <th className="w-28 px-3 py-3 font-medium">Status</th>
          <th className="w-24 px-3 py-3 font-medium">Priority</th>
          <th className="w-36 px-3 py-3 font-medium">Client</th>
          <th className="w-36 px-3 py-3 font-medium">Resource</th>
          <th className="w-36 px-3 py-3 font-medium">Created</th>
          <th className="w-44 px-3 py-3 font-medium">Actions</th>
        </tr>
      </thead>
      <tbody className="divide-y divide-border-slate/70">
        {jobs.map((job) => (
          <tr key={job.id} className="text-text-primary hover:bg-card-highlight/40">
            <td className="px-3 py-3">
              <div className="flex min-w-0 items-center gap-2">
                <button className="truncate font-mono text-xs text-text-primary hover:text-ion-cyan" title={job.id} onClick={() => onSelect?.(job)}>{job.id}</button>
                <CopyButton value={job.id} label="Job ID" onCopied={onCopied} />
              </div>
            </td>
            <td className="truncate px-3 py-3">{job.type}</td>
            <td className="px-3 py-3"><StatusBadge label={job.status} tone={job.status} /></td>
            <td className="px-3 py-3">{job.priority}</td>
            <td className="truncate px-3 py-3">{job.client || 'Open WebUI'}</td>
            <td className="truncate px-3 py-3 font-mono text-xs text-text-secondary">{job.resourceClass || job.resource_class || 'gpu_llm'}</td>
            <td className="truncate px-3 py-3 text-text-secondary">{formatDate(job.createdAt ?? job.created_at)}</td>
            <td className="px-3 py-3">
              <div className="flex gap-2">
                {job.status === 'failed' && onRetry && <CommandButton tone="blue" className="min-h-8 px-3 py-1 text-xs" onClick={() => onRetry(job.id)}>Retry</CommandButton>}
                <CommandButton tone="rose" className="min-h-8 px-3 py-1 text-xs" onClick={() => onCancel(job.id)}>Cancel</CommandButton>
              </div>
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  </div>
);
