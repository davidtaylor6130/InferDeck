import React, { useCallback, useState } from 'react';
import { getJobs, getStatus, isAuthenticationError } from '../api';
import { useDashboardAccess } from '../components/DashboardAccess';
import { useGateway } from '../gateway';
import type { JobRecord, StatusPayload } from '../types';
import { usePolling } from '../usePolling';
import { formatDuration } from '../utils';

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

export const RequestDetails: React.FC<{ job: JobRecord }> = ({ job }) => {
  const values = requestMeasurements(job);
  return <section className="border-t border-white/25 py-6" aria-label="Selected request">
    <div className="flex flex-wrap items-baseline justify-between gap-3"><h2 className="break-all font-semibold">{job.id}</h2><span>{duration(job.durationMs)} inference</span></div>
    <p className="mt-2 break-words text-sm">{job.resolvedModel || job.model} / {job.endpoint || job.type}</p>
    <dl className="my-6 grid grid-cols-2 gap-5 md:grid-cols-4">
      {([['Queue', job.queueDurationMs], ['Model loading', job.swapLoadDurationMs], ['Prompt processing', job.promptDurationMs], ['Generation', job.generationDurationMs]] as const).map(([label, value]) => <div key={label}><dt className="text-sm">{label}</dt><dd className="mt-1 font-mono">{stageDuration(value)}</dd></div>)}
    </dl>
    <p>{values.cacheReason}</p>
    <dl className="mt-4 grid gap-3 text-sm sm:grid-cols-2">
      <div><dt>Prompt reused</dt><dd className="font-mono">{values.cachePercent === undefined ? 'Not measured' : `${job.cachedPromptTokens!.toLocaleString()} / ${job.promptTokens.toLocaleString()} tokens (${values.cachePercent.toFixed(1)}%)`}</dd></div>
      <div><dt>Uncached prompt speed</dt><dd className="font-mono">{rate(values.promptRate)}</dd></div>
      <div><dt>Output speed</dt><dd className="font-mono">{rate(values.outputRate)}</dd></div>
      <div><dt>First token</dt><dd className="font-mono">{stageDuration(job.firstTokenDurationMs)}</dd></div>
    </dl>
    {job.status === 'failed' && <p role="status" className="mt-5 break-words text-danger-rose">{job.httpStatus === 499 ? 'Client disconnected or cancelled.' : `Request failed (HTTP ${job.httpStatus}).`}{job.errorCode ? ` ${job.errorCode}` : ''} This request is not retried here.</p>}
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
  return <div className="bg-black text-white">
    <div className="flex flex-wrap items-center justify-between gap-3"><h2 className="text-xl font-semibold">Requests</h2><button className="min-h-11 border border-white/25 px-3 text-sm" onClick={() => { void refresh(); }}>Refresh</button></div>
    {error && <p className="mt-4 text-warning-amber" role="status">{error}{snapshot ? ' Showing the last received data.' : ''}</p>}
    <dl className="my-6 grid grid-cols-2 gap-5 md:grid-cols-4">
      <div><dt className="text-sm">Running</dt><dd className="mt-1 text-lg tabular-nums">{(connection === 'connected' ? stats?.activeRequests ?? queue?.running : queue?.running) ?? 'Not measured'}</dd></div>
      <div><dt className="text-sm">Waiting</dt><dd className="mt-1 text-lg tabular-nums">{queue?.queued ?? 'Not measured'}</dd></div>
      <div><dt className="text-sm">Loaded model</dt><dd className="mt-1 break-words">{snapshot ? snapshot.status.current || 'None' : 'Not measured'}</dd></div>
      <div><dt className="text-sm">Model swaps</dt><dd className="mt-1 text-lg tabular-nums">{snapshot?.status.metrics.total_swaps ?? 'Not measured'}</dd></div>
    </dl>
    {waiting.length > 0 && <section className="border-t border-white/25 py-5" aria-label="Waiting requests"><h3 className="font-semibold">Waiting requests</h3><ul className="mt-3 divide-y divide-white/15">{waiting.map(item => <li key={item.id} className="flex flex-wrap justify-between gap-2 py-3"><span>Queue entry {item.id}: {item.model}</span><span>{duration(item.queuedMs)} waiting / position {item.position}</span></li>)}</ul>{queue?.resourceDecision && <p className="mt-3 text-sm">{queue.resourceDecision}</p>}</section>}
    <p className="mb-4 text-sm">Per-request timings appear after completion. Running requests are counted above.</p>
    {!snapshot ? <p role="status">{error ? 'Retry when the gateway is available.' : 'Loading requests...'}</p> : jobs.length === 0 ? <section className="border-t border-white/25 py-8"><h3 className="font-semibold">No completed requests</h3><p className="mt-2">Requests will appear here when a client finishes an inference.</p></section> : <>
      <div className="max-h-[28rem] overflow-auto"><table className="w-full table-fixed border-collapse text-left text-sm">
        <caption className="py-3 text-left font-semibold">Recent requests</caption>
        <thead className="sticky top-0 border-y border-white/25 bg-black"><tr><th className="w-[42%] px-2 py-3 md:w-[28%]" scope="col">Request</th><th className="px-2 py-3" scope="col">Result</th><th className="px-2 py-3" scope="col">Output tok/s</th><th className="hidden px-2 py-3 md:table-cell" scope="col">Queue</th><th className="hidden px-2 py-3 md:table-cell" scope="col">Load</th><th className="hidden px-2 py-3 md:table-cell" scope="col">PP</th><th className="hidden px-2 py-3 md:table-cell" scope="col">Prompt reused</th></tr></thead>
        <tbody>{jobs.map(job => { const key = rowKey(job); const values = requestMeasurements(job); return <tr key={key} className={`border-b border-white/15 ${chosen === job ? 'bg-white/[0.06]' : ''}`}><td className="px-2 py-3"><button className="min-h-11 max-w-full break-all text-left underline underline-offset-4" aria-pressed={chosen === job} onClick={() => setSelected(key)}>{job.id}</button></td><td className="break-words px-2 py-3">{job.status === 'succeeded' ? 'Completed' : job.httpStatus === 499 ? 'Cancelled' : 'Failed'}</td><td className="px-2 py-3 tabular-nums">{values.outputRate === undefined ? 'Not measured' : values.outputRate.toFixed(1)}</td><td className="hidden px-2 py-3 md:table-cell">{stageDuration(job.queueDurationMs)}</td><td className="hidden px-2 py-3 md:table-cell">{stageDuration(job.swapLoadDurationMs)}</td><td className="hidden px-2 py-3 md:table-cell">{stageDuration(job.promptDurationMs)}</td><td className="hidden px-2 py-3 md:table-cell">{values.cachePercent === undefined ? 'Not measured' : `${values.cachePercent.toFixed(1)}%`}</td></tr>; })}</tbody>
      </table></div>
      {chosen && <RequestDetails job={chosen} />}
    </>}
  </div>;
};
