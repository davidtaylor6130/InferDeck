import React, { useMemo, useState } from 'react';
import type { PageProps } from '../types';
import { EmptyState } from '../components/EmptyState';
import { MetricCard } from '../components/MetricCard';
import { QueueTable } from '../components/QueueTable';
import { SectionCard } from '../components/SectionCard';
import { CommandButton } from '../components/CommandButton';
import { getQueueCounts } from '../utils';

export const QueuePage: React.FC<PageProps> = ({ state, actions }) => {
  const [status, setStatus] = useState('all');
  const [query, setQuery] = useState('');
  const queue = getQueueCounts(state.statusData, state.jobsList);
  const filtered = useMemo(() => state.jobsList.filter(job => (status === 'all' || job.status === status) && `${job.id} ${job.type} ${job.client || ''}`.toLowerCase().includes(query.toLowerCase())), [state.jobsList, status, query]);

  return (
    <div className="space-y-5">
      <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-4">
        <MetricCard title="Queued" lines={[{ label: 'Waiting', value: queue.queued, tone: 'violet' }, { label: 'Running', value: queue.running }, { label: 'Paused', value: queue.paused }]} />
        <MetricCard title="Failures" lines={[{ label: 'Failed', value: queue.failed, tone: queue.failed ? 'rose' : 'muted' }, { label: 'Dead Letter', value: state.jobsList.filter(j => j.status === 'dead_letter').length }, { label: 'Retryable', value: state.jobsList.filter(j => j.status === 'failed').length }]} />
        <MetricCard title="GPU Lease" lines={[{ label: 'State', value: queue.gpuLocked ? 'Locked' : 'Free', tone: queue.gpuLocked ? 'blue' : 'mint' }, { label: 'Owner', value: queue.lockOwner || '—' }, { label: 'Policy', value: 'single_gpu_fifo' }]} />
        <MetricCard title="Scheduler" lines={[{ label: 'Mode', value: 'AI Mode', tone: 'violet' }, { label: 'Avg. Wait', value: '00:04:21' }, { label: 'Priority', value: 'Enabled' }]} />
      </div>
      <SectionCard title="Queue Management" action={<CommandButton tone="rose" onClick={() => window.confirm('Clear failed jobs?') && actions.clearFailedJobs()}>Clear Failed</CommandButton>}>
        <div className="mb-4 flex flex-wrap gap-3">
          <input className="min-h-10 min-w-64 rounded-lg border border-border-slate bg-deck-navy px-3 text-sm text-text-primary outline-none focus:border-infer-violet" placeholder="Search job ID, type, client" value={query} onChange={event => setQuery(event.target.value)} />
          <select className="min-h-10 rounded-lg border border-border-slate bg-deck-navy px-3 text-sm text-text-primary outline-none focus:border-infer-violet" value={status} onChange={event => setStatus(event.target.value)}>
            {['all', 'queued', 'running', 'paused', 'failed', 'succeeded', 'cancelled'].map(item => <option key={item} value={item}>{item}</option>)}
          </select>
          <CommandButton onClick={actions.refreshAll}>Refresh</CommandButton>
          <CommandButton tone="amber" onClick={actions.pauseQueue}>Pause Queue</CommandButton>
          <CommandButton tone="green" onClick={actions.resumeQueue}>Resume Queue</CommandButton>
        </div>
        {filtered.length ? <QueueTable jobs={filtered} onCancel={(id) => window.confirm(`Cancel running job ${id}?`) && actions.cancelJob(id)} onRetry={actions.retryJob} onCopied={actions.toast} /> : <EmptyState title="No jobs match the current filters." description="Queued, running, failed, and completed jobs will appear here when available." />}
      </SectionCard>
    </div>
  );
};
