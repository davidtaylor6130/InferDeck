import React, { useCallback, useState } from 'react';
import { getJobs, getStatus, isAuthenticationError } from '../api';
import { useDashboardAccess } from '../components/DashboardAccess';
import { Badge, Button, Dot, EmptyState, GroupHeader, GroupList, Notice, PageHeader, Readout, StatTile } from '../components/ui';
import { useGateway } from '../gateway';
import type { JobRecord, StatusPayload, Tone } from '../types';
import { usePolling } from '../usePolling';
import { formatDuration, timeAgo } from '../utils';

const measured = (value: number | undefined): value is number =>
  typeof value === 'number' && Number.isFinite(value) && value >= 0;
const duration = (value: number | undefined) => measured(value) ? formatDuration(value) : 'Not measured';
const stageDuration = (value: number | undefined) => measured(value) && value > 0 ? formatDuration(value) : 'Not recorded';
const rate = (value: number | undefined) => measured(value) ? `${value.toFixed(1)} tok/s` : 'Not measured';
const rowKey = (job: JobRecord) => `${job.id}:${job.timestampUnixMs}:${job.slotId}`;
export function requestMeasurements(job: JobRecord) {
  const text = job.modality === 'text' || (!job.modality && job.promptTokens > 0);
  const cacheKnown = text && measured(job.cachedPromptTokens) && job.promptTokens > 0 && measured(job.cacheWriteTokens) && job.cachedPromptTokens + job.cacheWriteTokens === job.promptTokens;
  return {
    outputRate: text && job.completionTokens > 0 && measured(job.generationDurationMs) && job.generationDurationMs > 0 && measured(job.tokensPerSecond) ? job.tokensPerSecond : undefined,
    promptRate: text && measured(job.promptDurationMs) && job.promptDurationMs > 0 && measured(job.promptTokensPerSecond) ? job.promptTokensPerSecond : undefined,
    cachePercent: cacheKnown && job.promptTokens > 0 ? job.cachedPromptTokens! * 100 / job.promptTokens : undefined,
    cacheReason: !text ? 'Prompt caching does not apply to this request.' : !cacheKnown ? 'Cache reuse was not measured.' : job.cachedPromptTokens! > 0 ? 'Matching prompt prefix reused.' : 'Prompt processed without cache reuse.',
  };
}

const resultTone = (job: JobRecord): Tone => job.status === 'succeeded' ? 'good' : job.httpStatus === 499 ? 'warn' : 'critical';
const resultLabel = (job: JobRecord) => job.status === 'succeeded' ? 'Completed' : job.httpStatus === 499 ? 'Cancelled' : 'Failed';
const STAGE_COLORS = ['rgb(var(--ink-3))', 'rgb(var(--violet))', 'rgb(var(--series-1))', 'rgb(var(--series-2))'];

export const RequestDetails: React.FC<{ job: JobRecord }> = ({ job }) => {
  const values = requestMeasurements(job);
  const stages = [['Queue', job.queueDurationMs], ['Model loading', job.swapLoadDurationMs], ['Prompt processing', job.promptDurationMs], ['Generation', job.generationDurationMs]] as const;
  const stageTotal = stages.reduce((sum, [, value]) => sum + (measured(value) ? value : 0), 0);
  return <section className="rounded-md border border-border-slate bg-panel-slate p-4" aria-label="Selected request">
    <div className="flex items-start gap-3.5">
      <div className="min-w-0 flex-1">
        <div className="flex flex-wrap items-baseline justify-between gap-x-3">
          <h2 className="break-all font-mono text-sm font-medium">{job.id}</h2>
          <span className="tabular text-sm font-semibold text-text-secondary">{duration(job.durationMs)} inference</span>
        </div>
        <p className="mt-0.5 break-words text-xs text-text-muted">{job.resolvedModel || job.model} / {job.endpoint || job.type}</p>
        <p className="mt-0.5 text-xs text-text-muted">API: {job.apiKeyName || (job.principalClass === 'managed_api_key' ? 'Legacy key (name not recorded)' : 'Shared / public API')}</p>
      </div>
    </div>
    {stageTotal > 0 && (
      <div className="mt-4 flex h-2 gap-0.5 overflow-hidden rounded-sm bg-elevated-slate" aria-hidden="true">
        {stages.map(([label, value], index) => measured(value) && value > 0 ? (
          <span key={label} style={{ width: `${value / stageTotal * 100}%`, background: STAGE_COLORS[index] }} />
        ) : null)}
      </div>
    )}
    <dl className="mt-4 grid grid-cols-2 gap-x-5 gap-y-3 md:grid-cols-4">
      {stages.map(([label, value], index) => (
        <div key={label}>
          <dt className="flex items-center gap-1.5 text-xs text-text-muted"><span className="h-2 w-2 rounded-full" style={{ background: STAGE_COLORS[index] }} />{label}</dt>
          <dd className="tabular mt-0.5 text-base font-semibold">{stageDuration(value)}</dd>
        </div>
      ))}
    </dl>
    <p className="mt-5 flex items-center gap-2 text-sm text-text-secondary"><Dot tone={values.cachePercent ? 'good' : 'idle'} />{values.cacheReason}</p>
    <dl className="mt-3 grid gap-px overflow-hidden rounded-md border border-border-slate bg-border-slate text-sm sm:grid-cols-2">
      <div className="bg-panel-slate px-3 py-2"><dt className="text-xs text-text-muted">Prompt reused</dt><dd className="tabular mt-0.5 font-medium">{values.cachePercent === undefined ? 'Not measured' : `${job.cachedPromptTokens!.toLocaleString()} / ${job.promptTokens.toLocaleString()} tokens (${values.cachePercent.toFixed(1)}%)`}</dd></div>
      <div className="bg-panel-slate px-3 py-2"><dt className="text-xs text-text-muted">Uncached prompt speed</dt><dd className="tabular mt-0.5 font-medium">{rate(values.promptRate)}</dd></div>
      <div className="bg-panel-slate px-3 py-2"><dt className="text-xs text-text-muted">Output speed</dt><dd className="tabular mt-0.5 font-medium">{rate(values.outputRate)}</dd></div>
      <div className="bg-panel-slate px-3 py-2"><dt className="text-xs text-text-muted">First token</dt><dd className="tabular mt-0.5 font-medium">{stageDuration(job.firstTokenDurationMs)}</dd></div>
    </dl>
    {job.status === 'failed' && <Notice tone="critical" role="status" className="mt-4">{job.httpStatus === 499 ? 'Client disconnected or cancelled.' : `Request failed (HTTP ${job.httpStatus}).`}{job.errorCode ? ` ${job.errorCode}` : ''} This request is not retried here.</Notice>}
  </section>;
};

export const RequestsPage: React.FC = () => {
  const { stats, connection } = useGateway();
  const access = useDashboardAccess();
  const [snapshot, setSnapshot] = useState<{ jobs: JobRecord[]; status: StatusPayload } | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const [error, setError] = useState('');
  const load = useCallback(async (signal: AbortSignal) => {
    const [jobs, status] = await Promise.all([getJobs(100, signal), getStatus(signal)]);
    return { jobs, status };
  }, []);
  const receive = useCallback((value: { jobs: JobRecord[]; status: StatusPayload }) => { setSnapshot(value); setError(''); }, []);
  const failed = useCallback((reason: unknown) => {
    if (isAuthenticationError(reason)) access?.requireSignIn();
    setError('Request data is unavailable.');
  }, [access?.requireSignIn]);
  const refresh = usePolling(load, receive, failed, 3000);
  const jobs = snapshot?.jobs ?? [];
  const chosen = jobs.find(job => rowKey(job) === selected) ?? jobs[0];
  const queue = snapshot?.status.queue;
  const waiting = queue?.requests ?? [];
  const running = (connection === 'connected' ? stats?.activeRequests ?? queue?.running : queue?.running);

  return <div className="space-y-8">
    <PageHeader
      title="Requests"
      subtitle="Per-request timings appear after a request completes. Running requests are counted below."
      actions={<Button onClick={() => { void refresh(); }}>Refresh</Button>}
    />
    {error && <Notice tone="warn" role="status">{error}{snapshot ? ' Showing the last received data.' : ''}</Notice>}
    <Readout aria-label="Request totals">
      <StatTile label="Running" value={running == null ? 'Not measured' : String(running)} />
      <StatTile label="Waiting" value={queue?.queued == null ? 'Not measured' : String(queue.queued)} />
      <StatTile label="Loaded model" value={snapshot ? snapshot.status.current || 'None' : 'Not measured'} />
      <StatTile label="Model swaps" value={snapshot?.status.metrics.total_swaps?.toLocaleString() ?? 'Not measured'} />
    </Readout>
    {waiting.length > 0 && <section aria-label="Waiting requests">
      <GroupHeader title="Waiting requests" aside={queue?.resourceDecision} />
      <GroupList>
        {waiting.map(item => <div key={item.id} className="flex flex-wrap items-center justify-between gap-2 px-3 py-2 text-sm"><span>Queue entry {item.id}: <span className="font-mono">{item.model}</span></span><span className="tabular text-text-muted">{duration(item.queuedMs)} waiting / position {item.position}</span></div>)}
      </GroupList>
    </section>}
    {!snapshot ? (
      <div className="space-y-2" role="status"><span className="sr-only">{error ? 'Retry when the gateway is available.' : 'Loading requests...'}</span>{[0, 1, 2, 3, 4].map(index => <div key={index} className="h-14 rounded-md bg-panel-slate" />)}</div>
    ) : jobs.length === 0 ? (
      <EmptyState title="No completed requests" detail="Requests appear here when a client finishes an inference." />
    ) : (
      <div className="grid items-start gap-6 xl:grid-cols-[minmax(0,1fr)_minmax(0,0.9fr)]">
        <section aria-label="Recent requests">
          <GroupHeader title="Recent requests" aside={`${jobs.length} latest`} />
          <div className="max-h-[68dvh] overflow-auto rounded-lg border border-border-slate bg-panel-slate shadow-card">
            <table className="w-full table-fixed border-collapse text-left text-sm">
              <caption className="sr-only">Recent requests</caption>
              <thead className="sticky top-0 z-10 text-xs text-text-muted">
                <tr>
                  <th className="w-[48%] px-3 py-2.5 font-medium md:w-[40%]" scope="col">Request</th>
                  <th className="px-2 py-2.5 font-medium" scope="col">Result</th>
                  <th className="px-2 py-2.5 text-right font-medium" scope="col">Output tok/s</th>
                  <th className="hidden px-2 py-2.5 text-right font-medium md:table-cell" scope="col">PP</th>
                  <th className="hidden px-3 py-2.5 text-right font-medium md:table-cell" scope="col">Prompt reused</th>
                </tr>
              </thead>
              <tbody className="tabular">
                {jobs.map(job => {
                  const key = rowKey(job);
                  const values = requestMeasurements(job);
                  const active = chosen === job;
                  return <tr key={key} onClick={() => setSelected(key)} className={`cursor-pointer border-t border-border-slate transition-colors ${active ? 'bg-elevated-slate shadow-[inset_2px_0_0_rgb(var(--accent))]' : 'hover:bg-panel-slate'}`}>
                    <td className="px-3 py-2">
                      <button className="block max-w-full truncate text-left font-mono text-text-primary" aria-pressed={active} onClick={() => setSelected(key)}>{job.id}</button>
                      <p className="truncate text-xs text-text-muted">{job.apiKeyName || (job.principalClass === 'managed_api_key' ? 'Legacy key' : 'Shared / public API')} · {timeAgo(job.timestampUnixMs)}</p>
                    </td>
                    <td className="px-2 py-2.5"><Badge label={resultLabel(job)} tone={resultTone(job)} /></td>
                    <td className="px-2 py-2.5 text-right font-semibold">{values.outputRate === undefined ? <span className="font-normal text-text-muted">Not measured</span> : values.outputRate.toFixed(1)}</td>
                    <td className="hidden px-2 py-2.5 text-right text-text-secondary md:table-cell">{stageDuration(job.promptDurationMs)}</td>
                    <td className="hidden px-3 py-2.5 text-right text-text-secondary md:table-cell">{values.cachePercent === undefined ? 'Not measured' : `${values.cachePercent.toFixed(1)}%`}</td>
                  </tr>;
                })}
              </tbody>
            </table>
          </div>
        </section>
        <div className="xl:sticky xl:top-4">
          <GroupHeader title="Request details" />
          {chosen && <RequestDetails job={chosen} />}
        </div>
      </div>
    )}
  </div>;
};
