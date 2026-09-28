import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { ChevronRightIcon } from '@heroicons/react/20/solid';
import { getJobs, getStatus, getPricing } from '../api';
import { UsageLineChart, UsageRangeTabs } from '../components/UsageCharts';
import { clientName, SlotStrip, SlotTable } from '../components/SlotGrid';
import { Badge, Button, Dot, EmptyState, Meter, ProgressBar, Readout, SectionTitle, Sparkline, StatTile } from '../components/ui';
import { modalityLabel } from '../dashboardSections';
import { modelHref } from '../routes';
import {
  ALL_MODELS,
  DEFAULT_COST_CONFIG,
  MODEL_COST_DEFAULTS_VERSION,
  TOKEN_RANGE_LABELS,
  buildCostDefaults,
  buildTokenSeries,
  getCostConfigForModel,
  loadCostConfig,
  saveCostConfig,
  type CostDefaults,
  type ModelCostConfig,
  type TokenRange,
} from '../cost';
import { mergeDailyUsage, useGateway } from '../gateway';
import {
  SUBSCRIPTION_SAVINGS_CHANGED_EVENT,
  calculateSavings,
  loadCancelledSubscriptions,
  loadIncludeApiCosts,
} from '../subscriptionSavings';
import { usePolling } from '../usePolling';
import type { JobRecord, StatusPayload } from '../types';
import {
  compactModel,
  formatCurrency,
  formatDuration,
  formatMb,
  formatTokenCount,
  formatUptime,
  temperatureTone,
  timeAgo,
} from '../utils';

export function overviewStatus(current: StatusPayload | null, live: StatusPayload | null): StatusPayload | null {
  if (!live || !current?.dailyTokenUsageAllTime) return live ?? current;
  return {
    ...live,
    dailyTokenUsage: mergeDailyUsage(current.dailyTokenUsage ?? [], live.dailyTokenUsage ?? []),
    dailyTokenUsageAllTime: true,
  };
}

export const OverviewPage: React.FC = () => {
  const { stats, statsHistory, status: gatewayStatus, models: gatewayModels, swap, activity, cancelSwap } = useGateway();
  const [liveStatus, setLiveStatus] = useState<StatusPayload | null>(null);
  const status = overviewStatus(gatewayStatus, liveStatus);
  const models = liveStatus?.models ?? gatewayModels;
  const live = status?.queue.liveRequests ?? [];
  const waiting = live.filter(request => request.slotId < 0);
  const history = statsHistory.slice(-60);
  const gpu = stats?.gpu;
  const summary = status?.summary;
  const running = status?.queue.running ?? stats?.activeRequests ?? 0;
  const queued = status?.queue.queued ?? 0;
  const loadedName = stats?.loadedModel || status?.current || models.find(model => model.primary || model.loaded)?.id || '';
  const tokensIn = summary?.promptTokens ?? stats?.lifetimeTokensIn ?? 0;
  const tokensOut = summary?.completionTokens ?? stats?.lifetimeTokensOut ?? 0;
  const totalLifetimeTokens = tokensIn + tokensOut;
  const [costDefaults, setCostDefaults] = useState<{ defaults: CostDefaults; fallback: ModelCostConfig }>({ defaults: {}, fallback: DEFAULT_COST_CONFIG });
  const [savedCosts, setSavedCosts] = useState<Record<string, ModelCostConfig>>({});
  const [usageRange, setUsageRange] = useState<TokenRange>('all');
  const [cancelError, setCancelError] = useState('');
  const [savingsRevision, setSavingsRevision] = useState(0);

  useEffect(() => {
    const refresh = () => setSavingsRevision(value => value + 1);
    window.addEventListener('storage', refresh);
    window.addEventListener(SUBSCRIPTION_SAVINGS_CHANGED_EVENT, refresh);
    return () => {
      window.removeEventListener('storage', refresh);
      window.removeEventListener(SUBSCRIPTION_SAVINGS_CHANGED_EVENT, refresh);
    };
  }, []);

  useEffect(() => {
    let active = true;
    getPricing().then(pricing => {
      if (!active) return;
      const built = buildCostDefaults(pricing);
      setCostDefaults(built);
      setSavedCosts(loadCostConfig(built.defaults, built.fallback));
    }).catch(() => {
      if (active) setSavedCosts(loadCostConfig({}, DEFAULT_COST_CONFIG));
    });
    return () => { active = false; };
  }, []);

  const lifetimeSeries = useMemo(
    () => buildTokenSeries(
      [],
      ALL_MODELS,
      DEFAULT_COST_CONFIG,
      status?.monthlyTokenUsage ?? [],
      savedCosts,
      costDefaults.defaults,
      costDefaults.fallback,
      'all',
      status?.dailyTokenUsage ?? [],
      status?.hourlyTokenUsage ?? [],
      Boolean(status?.dailyTokenUsageAllTime),
    ),
    [status?.monthlyTokenUsage, status?.dailyTokenUsage, status?.hourlyTokenUsage, status?.dailyTokenUsageAllTime, savedCosts, costDefaults],
  );
  const usageSeries = useMemo(
    () => buildTokenSeries(
      [],
      ALL_MODELS,
      DEFAULT_COST_CONFIG,
      status?.monthlyTokenUsage ?? [],
      savedCosts,
      costDefaults.defaults,
      costDefaults.fallback,
      usageRange,
      status?.dailyTokenUsage ?? [],
      status?.hourlyTokenUsage ?? [],
      Boolean(status?.dailyTokenUsageAllTime),
    ),
    [status?.monthlyTokenUsage, status?.dailyTokenUsage, status?.hourlyTokenUsage, status?.dailyTokenUsageAllTime, savedCosts, costDefaults, usageRange],
  );
  const totalCost = lifetimeSeries.cost.reduce((sum, value) => sum + value, 0);
  const portfolio = getCostConfigForModel(ALL_MODELS, savedCosts, costDefaults.defaults, costDefaults.fallback);
  const savings = useMemo(() => calculateSavings(
    loadCancelledSubscriptions(),
    {
      apiCostsCents: Math.max(0, Math.round(totalCost * 100)),
      includeApiCosts: loadIncludeApiCosts(),
      targetCents: Math.max(0, Math.round(portfolio.breakEvenTarget * 100)),
    },
  ), [portfolio.breakEvenTarget, savingsRevision, totalCost]);
  const totalSaved = savings.totalCents / 100;
  const roiRemaining = Math.max(0, portfolio.breakEvenTarget - totalSaved);
  const roiProgress = portfolio.breakEvenTarget > 0
    ? Math.min(100, totalSaved / portfolio.breakEvenTarget * 100)
    : 0;
  const runtimeLabel = swap.swapping
    ? 'Switching'
    : running > 0
      ? 'Processing'
      : queued > 0
        ? 'Queued'
        : loadedName
          ? 'Ready'
          : 'No model';
  const runtimeTone = swap.swapping || queued > 0 ? 'info' : running > 0 || loadedName ? 'good' : 'warn';
  const residentModels = models
    .filter(model => model.loaded && !model.alias)
    .sort((left, right) =>
      Number(Boolean(right.primary)) - Number(Boolean(left.primary)) ||
      Number(right.modality === 'text' || !right.modality) - Number(left.modality === 'text' || !left.modality) ||
      right.n_slots - left.n_slots);
  const [jobs, setJobs] = useState<JobRecord[]>([]);
  const [jobsError, setJobsError] = useState('');
  const loadJobs = useCallback(async (signal: AbortSignal) => {
    const [jobs, status] = await Promise.all([getJobs(8, signal), getStatus(signal)]);
    return { jobs, status };
  }, []);
  const receiveJobs = useCallback((value: { jobs: JobRecord[]; status: StatusPayload }) => { setJobs(value.jobs); setLiveStatus(value.status); setJobsError(''); }, []);
  const failJobs = useCallback(() => setJobsError('Live data unavailable. Showing last received data.'), []);
  usePolling(loadJobs, receiveJobs, failJobs, 2000);

  const persistBreakEvenTarget = (target: number) => {
    const merged = {
      ...savedCosts,
      [ALL_MODELS]: {
        ...portfolio,
        breakEvenTarget: target,
        defaultsVersion: MODEL_COST_DEFAULTS_VERSION,
      },
    };
    setSavedCosts(merged);
    saveCostConfig(merged);
  };

  const cancel = async () => {
    setCancelError('');
    const error = await cancelSwap();
    if (error) setCancelError(error);
  };

  const recent = [
    ...jobs.slice(0, 5).map(job => ({
      key: `${job.id}:${job.timestampUnixMs}`,
      tone: job.status === 'succeeded' ? 'good' as const : job.httpStatus === 499 ? 'idle' as const : 'critical' as const,
      headline: `${job.apiKeyName || (job.principalClass === 'managed_api_key' ? 'Legacy key' : 'Shared API')}: ${job.status === 'succeeded' ? 'completed' : job.httpStatus === 499 ? 'cancelled' : 'failed'} in ${formatDuration(job.durationMs)}`,
      detail: `${job.resolvedModel || job.model} · ${job.endpoint || job.type}`,
      at: job.timestampUnixMs,
    })),
    ...activity.filter(item => item.kind !== 'request').slice(0, Math.max(0, 5 - jobs.length)).map(item => ({
      key: item.id,
      tone: item.tone,
      headline: item.label,
      detail: item.detail,
      at: item.timestampUnixMs,
    })),
  ];

  const primary = residentModels.find(model => model.id === loadedName) ?? residentModels[0];
  const headline = swap.swapping
    ? `Switching to ${compactModel(swap.target)}`
    : running > 0
      ? `Serving ${running} request${running === 1 ? '' : 's'}`
      : queued > 0
        ? `${queued} request${queued === 1 ? '' : 's'} waiting`
        : residentModels.length
          ? 'Ready and idle'
          : 'No model loaded';
  const subline = [
    running > 0 && primary ? `on ${primary.id}` : residentModels.length && !swap.swapping ? `${residentModels.length} model${residentModels.length === 1 ? '' : 's'} loaded` : '',
    running > 0 && queued > 0 ? `${queued} waiting` : '',
    stats ? `up ${formatUptime(stats.uptimeSeconds)}` : 'waiting for gateway',
  ].filter(Boolean).join(' · ');
  const vramPercent = gpu?.vramTotalMb ? gpu.vramUsedMb / gpu.vramTotalMb * 100 : 0;
  const temperature = temperatureTone(gpu?.temperatureC);

  return (
    <div className="space-y-10">
      <section className="grid items-end gap-6 lg:grid-cols-[minmax(0,1fr)_320px]" aria-label="Runtime now">
        <div className="min-w-0">
          <p className="flex items-center gap-2 text-sm text-text-muted"><Dot tone={runtimeTone} /> {runtimeLabel}</p>
          <h1 className="mt-2 text-2xl font-semibold text-text-primary sm:text-[28px] sm:leading-9">{headline}</h1>
          <p className="mt-1 truncate text-sm text-text-muted">{subline}</p>
          {swap.swapping && (
            <div className="mt-3 flex max-w-md items-center gap-3">
              <div className="flex-1"><ProgressBar percent={0} tone="info" indeterminate /></div>
              <Button tone="danger" onClick={() => { void cancel(); }}>Cancel switch</Button>
            </div>
          )}
          {(cancelError || swap.lastError) && <p className="mt-2 line-clamp-2 text-sm text-danger-rose" role="alert">{cancelError || swap.lastError}</p>}
        </div>
        <div className="space-y-3" aria-label={gpu?.name || 'GPU'}>
          <Meter label="GPU utilization" value={gpu ? `${Math.round(gpu.utilizationPct)}%` : 'N/A'} percent={gpu?.utilizationPct ?? 0} />
          <Meter label="VRAM" value={gpu ? `${formatMb(gpu.vramUsedMb)}${gpu.vramTotalMb ? ` of ${formatMb(gpu.vramTotalMb)}` : ''}` : 'N/A'} percent={vramPercent} tone={vramPercent > 90 ? 'warn' : 'info'} />
          <Meter label="Temperature" value={gpu && gpu.temperatureC > 0 ? `${Math.round(gpu.temperatureC)}°C` : 'N/A'} percent={gpu?.temperatureC ?? 0} tone={temperature === 'warn' || temperature === 'critical' ? temperature : 'info'} />
        </div>
      </section>

      <section aria-label="Loaded models">
        <SectionTitle title="Loaded models" aside={String(residentModels.length)} />
        {residentModels.length === 0 ? (
          <div className="mt-2">
            <EmptyState
              title="Nothing is loaded"
              detail="Send a request and InferDeck loads the model on demand, or load one now."
              action={<a className="inline-flex min-h-8 items-center rounded-md bg-queue-blue px-3 text-sm font-semibold text-on-accent" href="#models/llm">Choose a model</a>}
            />
          </div>
        ) : (
          <div className="mt-2 divide-y divide-border-slate rounded-lg border border-border-slate bg-panel-slate shadow-card">
            {residentModels.map(model => {
              const slotted = live.filter(request => request.model === model.id && request.slotId >= 0);
              return (
                <div key={model.id} className="px-3 py-3">
                  <div className="flex flex-wrap items-center gap-x-6 gap-y-3 md:flex-nowrap">
                    <div className="min-w-0 flex-1">
                      <div className="flex min-w-0 items-center gap-2">
                        <a href={modelHref(model.id)} className="truncate font-mono text-sm font-medium text-text-primary hover:underline" title={model.id}>{model.id}</a>
                        {model.primary && <Badge label="Primary" tone="info" />}
                      </div>
                      <p className="mt-0.5 truncate text-xs text-text-muted">
                        {[modalityLabel(model.modality), model.modality === 'text' || !model.modality ? `${formatTokenCount(model.context_size)} context` : '', formatMb(model.vram_required_mb)].filter(Boolean).join(' · ')}
                      </p>
                    </div>
                    <div className="w-full md:w-[440px] md:shrink-0">
                      <SlotStrip model={model} requests={slotted} />
                    </div>
                  </div>
                  <SlotTable model={model} requests={slotted} />
                </div>
              );
            })}
          </div>
        )}
      </section>

      {(waiting.length > 0 || queued > 0) && (
        <section aria-label="Waiting requests">
          <SectionTitle title="Waiting" aside={`${queued} in queue`} />
          <div className="mt-2 divide-y divide-border-slate rounded-lg border border-border-slate bg-panel-slate shadow-card">
            {waiting.map(request => (
              <div key={`${request.id}:${request.startedUnixMs}`} className="flex flex-wrap items-center gap-x-3 gap-y-1 px-3 py-2">
                <span className="text-sm text-text-primary">{clientName(request)} <span className="text-2xs text-text-muted">p{request.priority}</span></span>
                <span className="min-w-0 flex-1 truncate text-xs text-text-muted">{request.phase === 'loading' ? 'Waiting for its model to load' : 'Waiting for a free slot'} on <span className="font-mono">{request.model}</span></span>
                <span className="tabular text-xs text-text-secondary">{formatDuration(request.elapsedMs)}</span>
              </div>
            ))}
          </div>
        </section>
      )}

      <div className="grid gap-10 xl:grid-cols-[minmax(0,1.5fr)_minmax(0,1fr)]">
        <section className="min-w-0">
          <SectionTitle title="Combined usage" aside={`${TOKEN_RANGE_LABELS[usageRange]}, all services`} action={<UsageRangeTabs value={usageRange} onChange={setUsageRange} />} />
          <UsageLineChart
            labels={usageSeries.months}
            series={[{ label: 'All services', color: 'rgb(var(--series-1))', values: usageSeries.requests }]}
            ariaLabel={`Combined requests across all services for ${TOKEN_RANGE_LABELS[usageRange]}`}
            height={140}
          />
          <p className="mt-3 text-sm text-text-secondary">
            Worth <span className="font-medium text-text-primary">{formatCurrency(totalCost)}</span> at hosted API prices
            {portfolio.breakEvenTarget > 0 ? `, ${formatCurrency(roiRemaining)} to break even.` : '.'}
          </p>
        </section>

        <section className="min-w-0" aria-label="Recent requests">
          <div className="flex min-h-8 items-center justify-between pb-2">
            <SectionTitle title="Recent activity" />
            <a href="#requests" className="text-sm text-queue-blue hover:underline">All requests</a>
          </div>
          {jobsError && <p role="status" className="pb-2 text-xs text-warning-amber">{jobsError}</p>}
          {recent.length > 0 && (
            <div className="divide-y divide-border-slate rounded-lg border border-border-slate bg-panel-slate px-3 shadow-card">
              {recent.map(item => (
                <div key={item.key} className="flex items-center gap-3 py-2">
                  <Dot tone={item.tone} />
                  <div className="min-w-0 flex-1">
                    <p className="truncate text-sm text-text-primary">{item.headline}</p>
                    <p className="truncate text-xs text-text-muted">{item.detail}</p>
                  </div>
                  <span className="shrink-0 text-xs text-text-muted">{timeAgo(item.at)}</span>
                </div>
              ))}
            </div>
          )}
          {!recent.length && !jobsError && <p className="text-sm text-text-muted">No activity yet.</p>}
        </section>
      </div>

      <details className="group border-t border-border-slate pt-4">
        <summary className="flex cursor-pointer list-none items-center gap-2 text-sm text-text-secondary hover:text-text-primary">
          <ChevronRightIcon className="h-4 w-4 text-text-muted transition-transform group-open:rotate-90" aria-hidden="true" />
          Runtime details
        </summary>
        <div className="mt-4 space-y-6">
          <Readout aria-label="Runtime totals">
            <StatTile label="Processing" value={String(running)} sub="active requests" />
            <StatTile label="Waiting" value={String(queued)} sub="queued requests" />
            <StatTile label="Lifetime requests" value={(summary?.totalRequests ?? stats?.totalRequests ?? 0).toLocaleString()} />
            <StatTile label="Lifetime tokens" value={formatTokenCount(totalLifetimeTokens)} />
            <div className="col-span-2 -ml-px -mt-px min-w-0 border-l border-t border-border-slate px-4 py-3 sm:col-span-1">
              <p className="truncate text-xs text-text-muted">API-equivalent value</p>
              <p className="tabular mt-1 text-xl font-semibold text-text-primary">{formatCurrency(totalCost)}</p>
              {portfolio.breakEvenTarget > 0 ? (
                <div className="mt-1.5">
                  <ProgressBar percent={roiProgress} tone="good" />
                  <p className="mt-1 truncate text-xs text-text-muted">{formatCurrency(roiRemaining)} to break even</p>
                </div>
              ) : <p className="mt-0.5 text-xs text-text-muted">vs. hosted APIs</p>}
            </div>
          </Readout>
          <div className="grid grid-cols-2 overflow-hidden rounded-lg border border-border-slate bg-panel-slate shadow-card lg:grid-cols-4 [&>*]:-ml-px [&>*]:-mt-px [&>*]:border-l [&>*]:border-t [&>*]:border-border-slate">
            <Sparkline label="GPU utilization" display={gpu ? `${Math.round(gpu.utilizationPct)}%` : 'N/A'} values={history.map(item => item.gpu.utilizationPct)} tone="info" yMax={100} />
            <Sparkline label="VRAM used" display={gpu ? formatMb(gpu.vramUsedMb) : 'N/A'} values={history.map(item => item.gpu.vramUsedMb)} tone="info" />
            <Sparkline label="GPU temperature" display={gpu && gpu.temperatureC > 0 ? `${Math.round(gpu.temperatureC)}°C` : 'N/A'} values={history.map(item => item.gpu.temperatureC)} tone={temperature} yMax={100} statusLabel />
            <Sparkline label="Average generation speed" display={`${(stats?.avgTokensPerSecond ?? 0).toFixed(1)} t/s`} values={history.map(item => item.avgTokensPerSecond)} tone="info" />
          </div>
          {status?.queue.resourceDecision && <p className="text-xs text-text-muted">Scheduler: {status.queue.resourceDecision}</p>}
          {status && !status.queue.liveRequests && <p role="status" className="text-xs text-warning-amber">This gateway needs the matching dashboard telemetry build.</p>}
          <label className="flex max-w-sm items-center justify-between gap-3 text-sm text-text-secondary">
            Break-even target (USD)
            <input
              className="tabular h-8 w-28 px-2 text-right text-sm"
              type="number"
              min="0"
              step="1"
              value={portfolio.breakEvenTarget}
              onChange={event => persistBreakEvenTarget(Number(event.target.value) || 0)}
            />
          </label>
          <p className="text-xs text-text-muted">{formatCurrency(totalSaved)} saved so far: {formatCurrency(savings.subscriptionCents / 100)} from cancelled subscriptions, API-equivalent value {savings.includedApiCosts ? 'included' : 'excluded'}.</p>
        </div>
      </details>
    </div>
  );
};
