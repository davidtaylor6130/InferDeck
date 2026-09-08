import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { getJobs, getStatus, getPricing } from '../api';
import { UsageLineChart, UsageRangeTabs } from '../components/UsageCharts';
import { Badge, Button, Panel, ProgressBar, SectionTitle, Sparkline, Stat } from '../components/ui';
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
import type { JobRecord, StatusPayload, LiveRequest } from '../types';
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

const ClientChip: React.FC<{ request: LiveRequest }> = ({ request }) => <span className="inline-flex max-w-full items-center gap-1.5 rounded border border-white/15 bg-white/[0.04] px-2 py-0.5 text-xs font-medium text-text-secondary">
  <span className={`h-2 w-2 shrink-0 rounded-full ${request.apiKeyId ? 'bg-queue-blue' : 'border border-dashed border-white/40'}`} />
  <span className="truncate" title={request.apiKeyName || 'Shared / public API'}>{request.apiKeyName || 'Shared / public API'}</span>
  <span className="shrink-0 text-[10px] text-text-muted">p{request.priority}</span>
</span>;

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
  const residentModels = models.filter(model => model.loaded && !model.alias);
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

  return (
    <div className="mx-auto max-w-[1280px] space-y-5 mock-home">
      <Panel className="border-t-0 pt-0">
        <SectionTitle title="Runtime now" aside={stats ? `up ${formatUptime(stats.uptimeSeconds)}` : 'waiting for gateway'} action={<Badge label={runtimeLabel} tone={runtimeTone} />} />
        <div className="mt-4 grid gap-5 lg:grid-cols-[minmax(0,1.3fr)_minmax(340px,0.7fr)] lg:items-start">
          <div className="min-w-0 border-l-2 border-queue-blue pl-4" aria-live="polite">
            <div className="flex flex-wrap items-center justify-between gap-2">
              <span className="text-xs font-medium text-text-muted">Resident models / {residentModels.length}</span>

            </div>
            <div className="divide-y divide-white/10">
              {residentModels.map(model => {
                const requests = live.filter(request => request.model === model.id && request.slotId >= 0);
                return <div key={model.id} className="py-1">
                  <details className="group">
                    <summary className="-mx-1 flex cursor-pointer list-none flex-wrap items-center gap-x-3 gap-y-1 rounded px-1 py-2.5 text-left hover:bg-white/[0.02]">
                      <span className="flex shrink-0 gap-1" aria-label={`${model.active_requests ?? 0} active, ${model.free_slots ?? 0} free slots`}>{Array.from({length: Math.min(model.n_slots, 32)}, (_, index) => <span key={index} className={`h-3 w-3 rounded-[3px] ${index < (model.active_requests ?? 0) ? 'bg-success-green shadow-[0_0_6px_rgba(82,183,136,0.55)]' : 'border border-dashed border-white/30'}`} />)}</span>
                      <span className="min-w-0 flex-1 truncate font-mono text-sm font-semibold" title={model.id}>{model.id}</span>
                      <Badge label={model.family || model.runtime || 'Model'} tone="idle" />
                      {model.primary && <Badge label="Primary" tone="info" />}
                      <span className="text-xs tabular-nums text-text-secondary">{model.active_requests ?? 0}/{model.n_slots} active</span>
                      <span className="text-xs text-text-muted group-open:rotate-90" aria-hidden="true">&#8250;</span>
                    </summary>
                    <p className="pb-2 text-xs text-text-muted">{formatTokenCount(model.context_size)} context / {model.free_slots ?? 'Unknown'} slots free / {formatMb(model.vram_required_mb)} estimated memory</p>
                  </details>
                  <div className="overflow-x-auto">
                    <table className="w-full table-fixed border-collapse text-left" aria-label={`${model.id} live slots`}>
                      <colgroup><col className="w-6"/><col/><col className="w-14 sm:w-[72px]"/><col className="w-14 sm:w-[72px]"/><col className="w-14 sm:w-16"/></colgroup>
                      <thead><tr className="border-t border-white/[0.07] text-[10px] font-medium uppercase tracking-wide text-text-muted"><th className="py-1.5 font-medium">#</th><th className="py-1.5 font-medium">Client (API key)</th><th className="text-right font-medium">PP tok/s</th><th className="text-right font-medium">TPS tok/s</th><th className="text-right font-medium">Elapsed</th></tr></thead>
                      <tbody>{requests.map(request => <tr key={`${request.id}:${request.startedUnixMs}`} className="border-t border-white/[0.07]" title={`${request.phase} / ${request.endpoint} / ${request.id}`}>
                        <td className="py-2 text-xs text-text-muted"><span className="sr-only">Slot </span>{request.slotId}</td>
                        <td className="min-w-0 py-2 pr-2"><ClientChip request={request}/><span className="sr-only">{request.requestedModel}</span></td>
                        <td className="py-2 text-right text-sm tabular-nums">{request.promptTokensPerSecond == null ? 'N/A' : request.promptTokensPerSecond.toFixed(0)}</td>
                        <td className="py-2 text-right text-sm tabular-nums">{request.tokensPerSecond == null ? 'N/A' : request.tokensPerSecond.toFixed(1)}</td>
                        <td className="py-2 text-right text-sm tabular-nums">{formatDuration(request.elapsedMs)}</td>
                      </tr>)}</tbody>
                    </table>
                  </div>
                </div>;
              })}
              {residentModels.length === 0 && <p className="py-4 text-sm text-text-muted">No models resident.</p>}
            </div>
            <div className="mt-3 border-t border-border-slate pt-3">
              <div className="flex justify-between gap-2"><h3 className="text-sm font-medium">Waiting</h3><span className="text-xs text-text-muted">{queued} queued</span></div>
              {waiting.map(request => <details key={`${request.id}:${request.startedUnixMs}`} className="group border-b border-white/10">
                <summary className="flex cursor-pointer list-none flex-wrap items-center gap-x-3 gap-y-1 py-2 text-left hover:bg-white/[0.02]">
                  <ClientChip request={request}/><span className="min-w-0 flex-1 truncate font-mono text-xs text-text-secondary">{request.model}</span><span className="text-xs tabular-nums text-text-muted">{formatDuration(request.elapsedMs)}</span><span className="text-text-muted group-open:rotate-90" aria-hidden="true">&#8250;</span>
                </summary>
                <p className="border-l border-queue-blue/40 py-2 pl-9 text-xs text-text-muted">{request.phase === 'loading' ? 'Waiting for model' : 'Waiting for an available slot'} / {request.endpoint} / {request.id}</p>
              </details>)}
              {queued === 0 && waiting.length === 0 && <p className="mt-2 text-xs text-text-muted">No waiting requests.</p>}
              {status && !status.queue.liveRequests && <p role="status" className="mt-2 text-xs text-warning-amber">This gateway needs the matching dashboard telemetry build.</p>}
            </div>
            {swap.swapping ? (
              <div className="mt-3 flex flex-wrap items-center gap-3">
                <div className="min-w-[180px] flex-1"><ProgressBar percent={0} tone="info" indeterminate /></div>
                <Button tone="danger" onClick={() => { void cancel(); }}>Cancel switch</Button>
              </div>
            ) : !loadedName ? (
              <a className="mt-3 inline-flex min-h-11 items-center rounded bg-queue-blue px-3 text-sm font-medium text-[#08111f] sm:min-h-10" href="#llm/models">Find a model</a>
            ) : null}
            {(cancelError || swap.lastError) && <p className="mt-2 text-xs text-danger-rose" role="alert">{cancelError || swap.lastError}</p>}
          </div>
          <div className="min-w-0 space-y-4">
            <div className="grid grid-cols-2 gap-5">
              <Stat label="Processing" value={String(running)} tone={running ? 'good' : 'idle'} sub="active requests" />
              <Stat label="Queued" value={String(queued)} tone={queued ? 'info' : 'idle'} sub="waiting requests" />
            </div>
            <div className="mock-telemetry grid grid-cols-2 gap-x-5 gap-y-4 border-t border-border-slate pt-4">
          <Sparkline
            label="GPU utilization"
            sub={gpu?.name}
            display={gpu ? `${Math.round(gpu.utilizationPct)}%` : 'N/A'}
            values={history.map(item => item.gpu.utilizationPct)}
            tone="info"
            yMax={100}
          />
          <Sparkline
            label="VRAM used"
            display={gpu ? formatMb(gpu.vramUsedMb) : 'N/A'}
            values={history.map(item => item.gpu.vramUsedMb)}
            tone="violet"
            sub={gpu?.vramTotalMb ? `of ${formatMb(gpu.vramTotalMb)}` : undefined}
          />
          <Sparkline
            label="GPU temperature"
            display={gpu && gpu.temperatureC > 0 ? `${Math.round(gpu.temperatureC)}°C` : 'N/A'}
            values={history.map(item => item.gpu.temperatureC)}
            tone={temperatureTone(gpu?.temperatureC)}
            yMax={100}
            statusLabel
          />
          <Sparkline
            label="Average generation speed"
            display={`${(stats?.avgTokensPerSecond ?? 0).toFixed(1)} t/s`}
            values={history.map(item => item.avgTokensPerSecond)}
            tone="good"
          />
            </div>
            <section className="border-t border-border-slate pt-4" aria-label="Recent requests">
              <SectionTitle title="Recent activity" aside="latest 3" />
              {jobsError && <p role="status" className="mt-2 text-xs text-warning-amber">{jobsError}</p>}
              <div className="divide-y divide-white/10">{jobs.slice(0, 3).map(job => <div key={`${job.id}:${job.timestampUnixMs}`} className="py-3">
                <div className="flex flex-wrap justify-between gap-2"><Badge label={job.status === 'succeeded' ? 'Completed' : job.httpStatus === 499 ? 'Cancelled' : 'Failed'} tone={job.status === 'succeeded' ? 'good' : 'critical'} /><span className="text-xs text-text-muted">{timeAgo(job.timestampUnixMs)}</span></div>
                <p className="mt-1 break-words font-mono text-sm">{job.resolvedModel || job.model}</p>
                <p className="mt-1 break-words text-xs text-text-muted">{job.endpoint || job.type} / {formatDuration(job.durationMs)}</p>
                <p className="mt-1 text-xs text-text-secondary">API: {job.apiKeyName || (job.principalClass === 'managed_api_key' ? 'Legacy key (name not recorded)' : 'Shared / public API')}</p>
              </div>)}</div>
              {activity.filter(item => item.kind !== 'request').slice(0, Math.max(0, 3 - jobs.length)).map(item => <div key={item.id} className="border-t border-white/10 py-3">
                <div className="flex justify-between gap-2"><Badge label="Model" tone={item.tone} /><span className="text-xs text-text-muted">{timeAgo(item.timestampUnixMs)}</span></div>
                <p className="mt-1 break-words text-sm">{item.label}</p>
                {item.detail && <p className="mt-1 break-words text-xs text-text-muted">{item.detail}</p>}
              </div>)}
              {!jobs.length && !activity.length && !jobsError && <p className="py-3 text-xs text-text-muted">No activity yet.</p>}
            </section>

          </div>
        </div>
        {status?.queue.resourceDecision && (
          <p className="mt-4 border-t border-border-slate pt-3 text-xs text-text-muted">Scheduler: {status.queue.resourceDecision}</p>
        )}
        <div className="mt-4 grid grid-cols-2 gap-4 border-t border-border-slate pt-4 sm:grid-cols-3">
          <Stat label="Lifetime requests" value={(summary?.totalRequests ?? stats?.totalRequests ?? 0).toLocaleString()} />
          <Stat label="Lifetime tokens" value={formatTokenCount(totalLifetimeTokens)} />
          <Stat label="API-equivalent value" value={formatCurrency(totalCost)} tone="good" />
        </div>
        {portfolio.breakEvenTarget > 0 && (
          <div className="mt-4">
            <div className="mb-1 flex justify-between gap-3 text-xs text-text-muted">
              <span>Break-even progress · {formatCurrency(totalSaved)} saved</span>
              <span>{formatCurrency(roiRemaining)} remaining</span>
            </div>
            <ProgressBar percent={roiProgress} tone="good" />
            <p className="mt-1 text-xs text-text-muted">
              {formatCurrency(savings.subscriptionCents / 100)} from cancelled subscriptions
              {' · '}{savings.includedApiCosts ? 'API-equivalent value included' : 'API-equivalent value excluded'}
            </p>
          </div>
        )}
        <details className="mt-3 border-t border-border-slate pt-3">
          <summary className="cursor-pointer text-xs font-medium text-text-secondary">Cost assumption</summary>
          <label className="mt-3 block max-w-xs text-xs text-text-muted">
            Break-even target (USD)
            <input
              className="mt-1 min-h-10 w-full border-white/10 bg-[#07101d] px-2 text-sm text-text-primary"
              type="number"
              min="0"
              step="1"
              value={portfolio.breakEvenTarget}
              onChange={event => persistBreakEvenTarget(Number(event.target.value) || 0)}
            />
          </label>
        </details>
      </Panel>


      <section className="grid gap-5 ">
        <Panel>
          <SectionTitle title="Combined usage" aside={`${TOKEN_RANGE_LABELS[usageRange]} · all services`} />
          <div className="mt-3"><UsageRangeTabs value={usageRange} onChange={setUsageRange} /></div>
          <UsageLineChart
            labels={usageSeries.months}
            series={[{ label: 'All services', color: '#72A7D8', values: usageSeries.requests }]}
            ariaLabel={`Combined requests across all services for ${TOKEN_RANGE_LABELS[usageRange]}`}
          />
        </Panel>


      </section>
    </div>
  );
};
