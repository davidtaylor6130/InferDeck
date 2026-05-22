import React from 'react';
import type { JobRecord } from '../types';
import { formatDate, stringifyPreview } from '../utils';
import { CommandButton } from './CommandButton';
import { CopyButton } from './CopyButton';
import { StatusBadge } from './StatusBadge';

interface JobDetailsDrawerProps {
  job: JobRecord | null;
  onClose: () => void;
  onCancel: (id: string) => void;
  onRetry: (id: string) => void;
  onCopied?: (message: string) => void;
}

export const JobDetailsDrawer: React.FC<JobDetailsDrawerProps> = ({ job, onClose, onCancel, onRetry, onCopied }) => {
  if (!job) return null;

  return (
    <div className="fixed inset-0 z-40">
      <button className="absolute inset-0 bg-black/60" onClick={onClose} aria-label="Close job details" />
      <aside className="absolute right-0 top-0 h-full w-full max-w-2xl overflow-y-auto border-l border-border-slate bg-deck-navy p-5 shadow-deck">
        <div className="flex items-start justify-between gap-4">
          <div className="min-w-0">
            <p className="text-xs uppercase tracking-wide text-text-muted">Job details</p>
            <div className="mt-1 flex min-w-0 items-center gap-2">
              <h2 className="truncate font-mono text-xl font-semibold text-text-primary" title={job.id}>{job.id}</h2>
              <CopyButton value={job.id} label="Job ID" onCopied={onCopied} />
            </div>
          </div>
          <button className="rounded-lg border border-border-slate px-3 py-2 text-text-secondary hover:text-text-primary" onClick={onClose}>Close</button>
        </div>

        <div className="mt-5 grid gap-3 sm:grid-cols-2">
          <Info label="Status" value={<StatusBadge label={job.status} tone={job.status} />} />
          <Info label="Type" value={job.type} />
          <Info label="Priority" value={job.priority} />
          <Info label="Client" value={job.client || 'Open WebUI'} />
          <Info label="Model" value={job.model || 'N/A'} mono />
          <Info label="Resource" value={job.resourceClass || job.resource_class || 'gpu_llm'} mono />
          <Info label="Created" value={formatDate(job.createdAt ?? job.created_at)} />
          <Info label="Started" value={formatDate(job.startedAt ?? job.started_at)} />
          <Info label="Tokens" value={job.totalTokens ?? 0} />
          <Info label="Duration" value={job.durationMs ? `${Math.round(job.durationMs)} ms` : 'N/A'} />
        </div>

        <div className="mt-5 flex flex-wrap gap-2">
          <CommandButton tone="blue" onClick={() => onRetry(job.id)}>Retry</CommandButton>
          <CommandButton tone="rose" onClick={() => onCancel(job.id)}>Cancel</CommandButton>
        </div>

        <Preview title="Payload Preview" value={job.payload ?? { model: job.model || 'qwen3.6:35b-a3b-q4_K_M', resource: job.resourceClass || 'gpu_llm' }} />
        <Preview title="Result Preview" value={job.result ?? 'No result captured yet.'} />
        <Preview title="Error Preview" value={job.error || 'No error recorded.'} />
        <Preview title="Event Timeline" value={(job.events && job.events.length > 0 ? job.events.map(event => `[${formatDate(event.createdAt)}] ${event.eventType || 'event'} ${event.message || ''}`).join('\n') : [
          `[${formatDate(job.createdAt ?? job.created_at)}] queued`,
          job.startedAt || job.started_at ? `[${formatDate(job.startedAt ?? job.started_at)}] started` : '[pending] awaiting GPU lease',
          job.completedAt || job.completed_at ? `[${formatDate(job.completedAt ?? job.completed_at)}] completed` : '[running] no terminal event',
        ].join('\n'))} />
      </aside>
    </div>
  );
};

const Info: React.FC<{ label: string; value: React.ReactNode; mono?: boolean }> = ({ label, value, mono }) => (
  <div className="min-w-0 rounded-lg border border-border-slate bg-panel-slate p-3">
    <p className="text-xs uppercase tracking-wide text-text-muted">{label}</p>
    <div className={`mt-2 truncate text-sm text-text-primary ${mono ? 'font-mono' : ''}`}>{value}</div>
  </div>
);

const Preview: React.FC<{ title: string; value: unknown }> = ({ title, value }) => (
  <section className="mt-5 rounded-lg border border-border-slate bg-panel-slate">
    <h3 className="border-b border-border-slate px-3 py-2 text-sm font-semibold text-text-primary">{title}</h3>
    <pre className="max-h-52 overflow-auto whitespace-pre-wrap p-3 font-mono text-xs leading-6 text-text-secondary">{stringifyPreview(value)}</pre>
  </section>
);
