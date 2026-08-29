import React, { useEffect, useMemo, useState } from 'react';
import { getPricing } from '../api';
import { UsageLineChart, UsageRangeTabs } from '../components/UsageCharts';
import { Badge, Button, EmptyState, Panel, ProgressBar, SectionTitle, Sparkline, Stat } from '../components/ui';
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
import { useGateway } from '../gateway';
import {
  compactModel,
  formatCurrency,
  formatDuration,
  formatMb,
  formatTokenCount,
  formatUptime,
  temperatureTone,
  timeAgo,
  toneText,
} from '../utils';

export const OverviewPage: React.FC = () => {
  const { stats, statsHistory, status, models, swap, activity, cancelSwap } = useGateway();
  const history = statsHistory.slice(-60);
  const gpu = stats?.gpu;
  const summary = status?.summary;
  const running = status?.queue.running ?? stats?.activeRequests ?? 0;
  const queued = status?.queue.queued ?? 0;
  const loadedName = stats?.loadedModel || status?.current || models.find(model => model.primary || model.loaded)?.id || '';
  const loadedModel = models.find(model => model.id === loadedName);
  const tokensIn = summary?.promptTokens ?? stats?.lifetimeTokensIn ?? 0;
  const tokensOut = summary?.completionTokens ?? stats?.lifetimeTokensOut ?? 0;
  const totalLifetimeTokens = tokensIn + tokensOut;
  const [costDefaults, setCostDefaults] = useState<{ defaults: CostDefaults; fallback: ModelCostConfig }>({ defaults: {}, fallback: DEFAULT_COST_CONFIG });
  const [savedCosts, setSavedCosts] = useState<Record<string, ModelCostConfig>>({});
  const [usageRange, setUsageRange] = useState<TokenRange>('all');
  const [cancelError, setCancelError] = useState('');

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
  const roiRemaining = Math.max(0, portfolio.breakEvenTarget - totalCost);
  const roiProgress = portfolio.breakEvenTarget > 0
    ? Math.min(100, totalCost / portfolio.breakEvenTarget * 100)
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
  const modelDetail = loadedModel
    ? [
        loadedModel.family,
        loadedModel.context_size ? `${formatTokenCount(loadedModel.context_size)} context` : '',
        loadedModel.free_slots != null ? `${loadedModel.free_slots}/${loadedModel.n_slots} slots free` : `${loadedModel.n_slots} slot${loadedModel.n_slots === 1 ? '' : 's'}`,
      ].filter(Boolean).join(' · ')
    : 'No language model is currently resident.';

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
    <div className="space-y-5">
      <Panel className="border-t-0 pt-0">
        <SectionTitle title="Runtime now" aside={stats ? `up ${formatUptime(stats.uptimeSeconds)}` : 'waiting for gateway'} />
        <div className="mt-4 grid gap-5 lg:grid-cols-[minmax(0,1.3fr)_minmax(360px,0.7fr)] lg:items-start">
          <div className="min-w-0 border-l-2 border-queue-blue pl-4" aria-live="polite">
            <div className="flex flex-wrap items-center gap-2">
              <span className="text-xs font-medium text-text-muted">Current model</span>
              <Badge label={runtimeLabel} tone={runtimeTone} />
            </div>
            <p className="mt-1 break-words font-mono text-xl font-semibold text-text-primary sm:text-2xl">
              {compactModel(swap.swapping ? swap.target : loadedName) || 'None loaded'}
            </p>
            <p className="mt-1 text-xs text-text-muted">
              {swap.swapping ? `Replacing ${compactModel(swap.from) || 'the current model'}` : modelDetail}
            </p>
            {swap.swapping ? (
              <div className="mt-3 flex flex-wrap items-center gap-3">
                <div className="min-w-[180px] flex-1"><ProgressBar percent={0} tone="info" indeterminate /></div>
                <Button tone="danger" onClick={() => { void cancel(); }}>Cancel switch</Button>
              </div>
            ) : !loadedName ? (
              <a className="mt-3 inline-flex min-h-10 items-center rounded bg-queue-blue px-3 text-sm font-medium text-[#08111f]" href="#llm/models">Find a model</a>
            ) : null}
            {(cancelError || swap.lastError) && <p className="mt-2 text-xs text-danger-rose" role="alert">{cancelError || swap.lastError}</p>}
          </div>
          <div className="grid grid-cols-2 gap-x-5 gap-y-4">
            <Stat label="Processing" value={String(running)} tone={running ? 'good' : 'idle'} sub={running === 1 ? 'active request' : 'active requests'} />
            <Stat label="Queued" value={String(queued)} tone={queued ? 'info' : 'idle'} sub={queued === 1 ? 'waiting request' : 'waiting requests'} />
            <Stat label="GPU" value={gpu ? `${Math.round(gpu.utilizationPct)}%` : 'N/A'} sub={gpu?.name || 'telemetry unavailable'} />
            <Stat label="VRAM" value={gpu ? formatMb(gpu.vramUsedMb) : 'N/A'} sub={gpu?.vramTotalMb ? `of ${formatMb(gpu.vramTotalMb)}` : undefined} />
          </div>
        </div>
        {status?.queue.resourceDecision && (
          <p className="mt-4 border-t border-border-slate pt-3 text-xs text-text-muted">Scheduler: {status.queue.resourceDecision}</p>
        )}
        <div className="mt-4 grid grid-cols-2 gap-4 border-t border-border-slate pt-4 sm:grid-cols-4">
          <Stat label="Lifetime requests" value={(summary?.totalRequests ?? stats?.totalRequests ?? 0).toLocaleString()} />
          <Stat label="Lifetime tokens" value={formatTokenCount(totalLifetimeTokens)} />
          <Stat label="p95 latency" value={formatDuration(summary?.p95LatencyMs)} />
          <Stat label="API-equivalent value" value={formatCurrency(totalCost)} tone="good" />
        </div>
        {portfolio.breakEvenTarget > 0 && (
          <div className="mt-4">
            <div className="mb-1 flex justify-between gap-3 text-xs text-text-muted">
              <span>Break-even progress</span>
              <span>{formatCurrency(roiRemaining)} remaining</span>
            </div>
            <ProgressBar percent={roiProgress} tone="good" />
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

      <Panel>
        <SectionTitle title="Live system" aside="last 60 seconds" />
        <div className="mt-4 grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
          <Sparkline
            label="GPU utilization"
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
            label="Generation speed"
            display={`${(stats?.avgTokensPerSecond ?? 0).toFixed(1)} t/s`}
            values={history.map(item => item.avgTokensPerSecond)}
            tone="good"
          />
        </div>
      </Panel>

      <section className="grid gap-5 xl:grid-cols-[minmax(0,1.4fr)_minmax(320px,0.6fr)]">
        <Panel>
          <SectionTitle title="Combined usage" aside={`${TOKEN_RANGE_LABELS[usageRange]} · all services`} />
          <div className="mt-3"><UsageRangeTabs value={usageRange} onChange={setUsageRange} /></div>
          <UsageLineChart
            labels={usageSeries.months}
            series={[{ label: 'All services', color: '#72A7D8', values: usageSeries.requests }]}
            ariaLabel={`Combined requests across all services for ${TOKEN_RANGE_LABELS[usageRange]}`}
          />
        </Panel>

        <Panel>
          <SectionTitle title="Recent activity" aside="latest 8" />
          <div className="mt-2 divide-y divide-white/10">
            {activity.length === 0 ? (
              <div className="py-4">
                <EmptyState title="No live activity yet" detail="New requests and model changes will appear here." />
              </div>
            ) : (
              activity.slice(0, 8).map(item => (
                <div key={item.id} className="grid min-w-0 grid-cols-[auto_minmax(0,1fr)] gap-x-3 gap-y-1 py-2.5">
                  <Badge
                    label={item.tone === 'critical' ? 'Failed' : item.tone === 'good' ? 'Completed' : item.tone === 'info' ? 'In progress' : 'Status'}
                    tone={item.tone}
                  />
                  <span className="justify-self-end text-xs text-text-muted">{timeAgo(item.timestampUnixMs)}</span>
                  <div className="col-span-2 min-w-0">
                    <p className="truncate text-sm text-text-primary">{item.label}</p>
                    {item.detail && <p className={`truncate text-xs ${item.tone === 'critical' ? toneText('critical') : 'text-text-muted'}`}>{item.detail}</p>}
                  </div>
                </div>
              ))
            )}
          </div>
        </Panel>
      </section>
    </div>
  );
};
