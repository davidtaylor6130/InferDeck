import React, { useCallback, useState } from 'react';
import { useFeedback } from '../components/Feedback';
import { ArrowLeftIcon } from '@heroicons/react/20/solid';
import { getStatus } from '../api';
import { SlotStrip, SlotTable } from '../components/SlotGrid';
import { Badge, Button, EmptyState, Notice } from '../components/ui';
import { modalityLabel, sectionForModality, sectionLabel } from '../dashboardSections';
import { useGateway } from '../gateway';
import { modelHref, type ModelTab } from '../routes';
import type { LiveRequest, StatusPayload } from '../types';
import { usePolling } from '../usePolling';
import { formatMb, formatTokenCount, timeAgo } from '../utils';
import { ModelSettingsPanel } from './OperatePage';

export const ModelDetailPage: React.FC<{ id: string; tab?: ModelTab }> = ({ id, tab }) => {
  const { models, status, swap, swapTo, unload } = useGateway();
  const { toast } = useFeedback();
  const model = models.find(entry => entry.id === id);
  const [live, setLive] = useState<LiveRequest[]>([]);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState('');
  const loadStatus = useCallback((signal: AbortSignal) => getStatus(signal), []);
  const receive = useCallback((next: StatusPayload) => setLive(next.queue.liveRequests ?? []), []);
  const fail = useCallback(() => {}, []);
  usePolling(loadStatus, receive, fail, 2000);

  if (!model) {
    return (
      <div className="space-y-6">
        <BackLink section="llm" />
        <EmptyState
          title={models.length ? `No model named ${id}` : 'Loading models...'}
          detail={models.length ? 'It may have been removed from the active profile.' : undefined}
          action={models.length ? <a className="text-sm text-queue-blue hover:underline" href="#models/llm">Back to models</a> : undefined}
        />
      </div>
    );
  }

  const section = sectionForModality(model.modality);
  const usage = (status?.tokenUsage ?? []).find(row => row.model === model.id);
  const slotted = live.filter(request => request.model === model.id && request.slotId >= 0);
  const waiting = live.filter(request => request.model === model.id && request.slotId < 0);
  const loading = swap.swapping && swap.target === model.id;
  const busy = model.active_requests ?? slotted.length;

  const run = async (action: () => Promise<string | null>, success: string) => {
    setPending(true);
    setError('');
    const failure = await action();
    if (failure) setError(failure);
    else toast(success, { tone: 'info' });
    setPending(false);
  };

  const state = model.runtime_available === false
    ? 'This runtime is not available in this build.'
    : loading
      ? 'Loading into memory now.'
      : model.loaded
        ? busy
          ? `Serving ${busy} request${busy === 1 ? '' : 's'} on ${model.n_slots} slot${model.n_slots === 1 ? '' : 's'}${waiting.length ? `, ${waiting.length} waiting` : ''}.`
          : `Loaded and idle, ready on ${model.n_slots} slot${model.n_slots === 1 ? '' : 's'}.`
        : 'Not loaded. It loads automatically when a request asks for it, or you can load it now.';
  const history = usage?.requests
    ? [
        `${usage.requests.toLocaleString()} request${usage.requests === 1 ? '' : 's'} so far`,
        usage.lastTimestampUnixMs ? `last used ${timeAgo(usage.lastTimestampUnixMs)}` : '',
        usage.avgTokensPerSecond ? `averaging ${usage.avgTokensPerSecond.toFixed(1)} tokens per second` : '',
      ].filter(Boolean).join(', ') + '.'
    : 'No requests recorded yet.';

  return (
    <div className="space-y-8">
      <BackLink section={section} />
      <header className="flex flex-wrap items-start justify-between gap-4">
        <div className="min-w-0">
          <div className="flex flex-wrap items-center gap-2">
            <h1 className="break-all font-mono text-xl font-medium text-text-primary">{model.id}</h1>
            {model.primary && <Badge label="Primary" tone="info" />}
            {model.optimization?.status === 'measured' && <Badge label="Measured optimized" tone="good" />}
          </div>
          <p className="mt-1 text-sm text-text-muted">
            {[modalityLabel(model.modality), model.family, section === 'llm' && model.context_size ? `${formatTokenCount(model.context_size)} context per slot` : '', model.vram_required_mb ? `${formatMb(model.vram_required_mb)} of memory` : '', model.has_vision ? 'Vision' : ''].filter(Boolean).join(' · ')}
          </p>
        </div>
        {model.runtime_available !== false && (
          <div className="flex items-center gap-2">
            {section === 'llm' && tab !== 'optimize' && (
              <a href={modelHref(model.id, 'optimize')} title="Benchmark safe profiles and recommend faster settings" className="inline-flex min-h-10 items-center gap-1.5 rounded-md px-3 text-sm font-medium text-text-secondary hover:bg-elevated-slate hover:text-text-primary sm:min-h-8">Auto-optimize</a>
            )}
            {model.loaded
              ? <Button disabled={pending} loading={pending && model.loaded} onClick={() => { void run(() => unload(model.id), `${model.id} unloaded`); }}>Unload</Button>
              : <Button tone="blue" disabled={pending || swap.swapping} loading={pending || loading} onClick={() => { void run(() => swapTo(model.id), `Loading ${model.id}`); }}>{loading ? 'Loading' : 'Load now'}</Button>}
          </div>
        )}
      </header>

      {error && <Notice tone="critical" role="alert">{error}</Notice>}

      <section>
        <p className="text-lg text-text-primary">{state}</p>
        <p className="mt-1 text-sm text-text-muted">{history}</p>
        {model.loaded && (
          <div className="mt-4">
            <SlotStrip model={model} requests={slotted} wide />
            <SlotTable model={model} requests={slotted} />
          </div>
        )}
      </section>

      <ModelSettingsPanel key={model.id} model={model} section={section} initialTab={tab} />
    </div>
  );
};

const BackLink: React.FC<{ section: ReturnType<typeof sectionForModality> }> = ({ section }) => (
  <a href={`#models/${section}`} className="inline-flex items-center gap-1 text-sm text-text-muted hover:text-text-primary">
    <ArrowLeftIcon className="h-4 w-4" aria-hidden="true" />
    {sectionLabel(section)} models
  </a>
);
