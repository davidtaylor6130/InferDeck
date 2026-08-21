import React, { useEffect, useMemo, useState } from 'react';
import { getPricing } from '../api';
import { UsageLineChart, UsageRangeTabs } from '../components/UsageCharts';
import { Badge, EmptyState, Panel, ProgressBar, SectionTitle, Sparkline, Stat } from '../components/ui';
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
import { bucketUsageForSection, modelsForSection, usageForSection } from '../dashboardSections';
import { useGateway } from '../gateway';
import type { MonthlyUsageRow } from '../types';
import {
  formatDuration,
  formatCurrency,
  formatMb,
  formatTokenCount,
  formatUptime,
  temperatureTone,
  timeAgo,
  toneText,
} from '../utils';

export const OverviewPage: React.FC = () => {
  const { stats, statsHistory, status, models, swap, activity } = useGateway();
  const history = statsHistory.slice(-60);
  const gpu = stats?.gpu;
  const summary = status?.summary;
  const llmModels = useMemo(() => modelsForSection(models, 'llm'), [models]);
  const dictationModels = useMemo(() => modelsForSection(models, 'dictation'), [models]);
  const llmUsage = useMemo(
    () => usageForSection(status?.tokenUsage ?? [], models, 'llm'),
    [status?.tokenUsage, models],
  );
  const dictationUsage = useMemo(
    () => usageForSection(status?.tokenUsage ?? [], models, 'dictation'),
    [status?.tokenUsage, models],
  );
  const [costDefaults, setCostDefaults] = useState<{ defaults: CostDefaults; fallback: ModelCostConfig }>({ defaults: {}, fallback: DEFAULT_COST_CONFIG });
  const [savedCosts, setSavedCosts] = useState<Record<string, ModelCostConfig>>({});
  const [usageRange, setUsageRange] = useState<TokenRange>('all');

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

  const llmRequests = llmUsage.reduce((sum, row) => sum + row.requests, 0);
  const llmTokens = llmUsage.reduce((sum, row) => sum + row.totalTokens, 0);
  const dictationRequests = dictationUsage.reduce((sum, row) => sum + row.requests, 0);
  const dictationSuccessful = dictationUsage.reduce((sum, row) => sum + row.successfulRequests, 0);
  const loadedLlm = llmModels.filter(model => model.loaded);
  const loadedDictation = dictationModels.filter(model => model.loaded);
  const tokensIn = summary?.promptTokens ?? stats?.lifetimeTokensIn ?? 0;
  const tokensOut = summary?.completionTokens ?? stats?.lifetimeTokensOut ?? 0;
  const totalLifetimeTokens = tokensIn + tokensOut;
  const llmMonthly = useMemo(
    () => withSharedBuckets(
      bucketUsageForSection(status?.monthlyTokenUsage ?? [], models, 'llm'),
      status?.monthlyTokenUsage ?? [],
    ),
    [status?.monthlyTokenUsage, models],
  );
  const dictationMonthly = useMemo(
    () => withSharedBuckets(
      bucketUsageForSection(status?.monthlyTokenUsage ?? [], models, 'dictation'),
      status?.monthlyTokenUsage ?? [],
    ),
    [status?.monthlyTokenUsage, models],
  );
  const llmDaily = useMemo(
    () => withSharedBuckets(
      bucketUsageForSection(status?.dailyTokenUsage ?? [], models, 'llm'),
      status?.dailyTokenUsage ?? [],
    ),
    [status?.dailyTokenUsage, models],
  );
  const dictationDaily = useMemo(
    () => withSharedBuckets(
      bucketUsageForSection(status?.dailyTokenUsage ?? [], models, 'dictation'),
      status?.dailyTokenUsage ?? [],
    ),
    [status?.dailyTokenUsage, models],
  );
  const llmHourly = useMemo(
    () => withSharedBuckets(
      bucketUsageForSection(status?.hourlyTokenUsage ?? [], models, 'llm'),
      status?.hourlyTokenUsage ?? [],
    ),
    [status?.hourlyTokenUsage, models],
  );
  const dictationHourly = useMemo(
    () => withSharedBuckets(
      bucketUsageForSection(status?.hourlyTokenUsage ?? [], models, 'dictation'),
      status?.hourlyTokenUsage ?? [],
    ),
    [status?.hourlyTokenUsage, models],
  );
  const llmLifetimeSeries = useMemo(
    () => buildTokenSeries(
      [], ALL_MODELS, DEFAULT_COST_CONFIG, llmMonthly, {},
      costDefaults.defaults, costDefaults.fallback, 'all', llmDaily, llmHourly,
      Boolean(status?.dailyTokenUsageAllTime),
    ),
    [llmMonthly, costDefaults, llmDaily, llmHourly, status?.dailyTokenUsageAllTime],
  );
  const dictationLifetimeSeries = useMemo(
    () => buildTokenSeries(
      [], ALL_MODELS, DEFAULT_COST_CONFIG, dictationMonthly, savedCosts,
      costDefaults.defaults, costDefaults.fallback, 'all', dictationDaily,
      dictationHourly, Boolean(status?.dailyTokenUsageAllTime),
    ),
    [dictationMonthly, savedCosts, costDefaults, dictationDaily, dictationHourly, status?.dailyTokenUsageAllTime],
  );
  const llmCost = llmLifetimeSeries.cost.reduce((sum, value) => sum + value, 0);
  const dictationCost = dictationLifetimeSeries.cost.reduce((sum, value) => sum + value, 0);
  const totalCost = llmCost + dictationCost;
  const portfolio = getCostConfigForModel(ALL_MODELS, savedCosts, costDefaults.defaults, costDefaults.fallback);
  const roiRemaining = Math.max(0, portfolio.breakEvenTarget - totalCost);
  const roiProgress = portfolio.breakEvenTarget > 0
    ? Math.min(100, totalCost / portfolio.breakEvenTarget * 100)
    : 0;
  const llmSeries = useMemo(
    () => buildTokenSeries(
      [],
      ALL_MODELS,
      DEFAULT_COST_CONFIG,
      llmMonthly,
      {},
      costDefaults.defaults,
      costDefaults.fallback,
      usageRange,
      llmDaily,
      llmHourly,
      Boolean(status?.dailyTokenUsageAllTime),
    ),
    [llmMonthly, costDefaults, usageRange, llmDaily, llmHourly, status?.dailyTokenUsageAllTime],
  );
  const dictationSeries = useMemo(
    () => buildTokenSeries(
      [],
      ALL_MODELS,
      DEFAULT_COST_CONFIG,
      dictationMonthly,
      savedCosts,
      costDefaults.defaults,
      costDefaults.fallback,
      usageRange,
      dictationDaily,
      dictationHourly,
      Boolean(status?.dailyTokenUsageAllTime),
    ),
    [dictationMonthly, savedCosts, costDefaults, usageRange, dictationDaily, dictationHourly, status?.dailyTokenUsageAllTime],
  );

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

  return (
    <div className="space-y-4">
      <Panel>
        <SectionTitle title="Everything at a glance" aside={stats ? `up ${formatUptime(stats.uptimeSeconds)}` : 'global'} />
        <p className="mt-2 max-w-3xl text-sm text-text-secondary">
          Aggregate health and persisted usage across language, speech-to-text, and text-to-speech services.
        </p>
        <div className="mt-4 grid grid-cols-2 gap-4 sm:grid-cols-4 xl:grid-cols-8">
          <Stat label="Requests" value={(summary?.totalRequests ?? stats?.totalRequests ?? 0).toLocaleString()} />
          <Stat label="Tokens in" value={formatTokenCount(tokensIn)} sub={tokenShare(tokensIn, totalLifetimeTokens)} />
          <Stat label="Tokens out" value={formatTokenCount(tokensOut)} sub={tokenShare(tokensOut, totalLifetimeTokens)} />
          <Stat label="Request avg t/s" value={(stats?.avgTokensPerSecond ?? 0).toFixed(1)} />
          <Stat label="Running" value={String(status?.queue.running ?? stats?.activeRequests ?? 0)} />
          <Stat label="Queued" value={String(status?.queue.queued ?? 0)} />
          <Stat label="p95 latency" value={formatDuration(summary?.p95LatencyMs)} />
          <Stat label="API-equivalent value" value={formatCurrency(totalCost)} sub={portfolio.breakEvenTarget > 0 ? `${roiProgress.toFixed(1)}% to break-even` : undefined} tone="good" />
        </div>
        {portfolio.breakEvenTarget > 0 && (
          <div className="mt-4">
            <div className="mb-1 flex justify-between text-xs text-text-muted">
              <span>Portfolio break-even progress</span>
              <span>{formatCurrency(roiRemaining)} remaining</span>
            </div>
            <ProgressBar percent={roiProgress} tone="good" />
          </div>
        )}
        <details className="mt-3 border-t border-border-slate pt-3">
          <summary className="cursor-pointer text-xs font-medium text-text-secondary">Portfolio assumptions</summary>
          <label className="mt-3 block max-w-xs text-xs text-text-muted">
            Break-even target (USD)
            <input
              className="mt-1 h-9 w-full rounded border border-white/10 bg-[#07101d] px-2 text-sm text-text-primary"
              type="number"
              min="0"
              step="1"
              value={portfolio.breakEvenTarget}
              onChange={event => persistBreakEvenTarget(Number(event.target.value) || 0)}
            />
          </label>
        </details>
      </Panel>

      <section className="grid gap-4 xl:grid-cols-2">
        <ServiceSummary
          title="LLM"
          href="#llm/settings"
          registered={llmModels.length}
          loaded={loadedLlm.length}
          requests={llmRequests}
          usageLabel="Lifetime tokens"
          usageValue={formatTokenCount(llmTokens)}
          costValue={formatCurrency(llmCost)}
          detail={loadedLlm.length
            ? `${loadedLlm.reduce((sum, model) => sum + (model.free_slots ?? 0), 0)} free slots across loaded models`
            : 'No language model currently loaded'}
        />
        <ServiceSummary
          title="Dictation"
          href="#dictation/settings"
          registered={dictationModels.length}
          loaded={loadedDictation.length}
          requests={dictationRequests}
          usageLabel="Successful requests"
          usageValue={dictationSuccessful.toLocaleString()}
          costValue={formatCurrency(dictationCost)}
          detail={`${dictationModels.filter(model => model.modality === 'audio_transcription').length} speech-to-text · ${dictationModels.filter(model => model.modality === 'audio_speech').length} text-to-speech`}
          dictation
        />
      </section>

      <Panel>
        <SectionTitle title="Live system" aside="last 60 seconds" />
        <div className="mt-3 flex flex-wrap gap-x-6 gap-y-2 border-b border-border-slate pb-3 text-xs text-text-secondary">
          <span><strong className="text-text-primary">{status?.queue.running ?? stats?.activeRequests ?? 0}</strong> running</span>
          <span><strong className="text-text-primary">{status?.queue.queued ?? 0}</strong> queued</span>
          {status?.queue.resourceDecision && <span className="max-w-[58ch] text-text-muted">{status.queue.resourceDecision}</span>}
        </div>
        <div className="mt-3 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
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
            label="GPU temp"
            display={gpu && gpu.temperatureC > 0 ? `${Math.round(gpu.temperatureC)}°C` : 'N/A'}
            values={history.map(item => item.gpu.temperatureC)}
            tone={temperatureTone(gpu?.temperatureC)}
            yMax={100}
            statusLabel
          />
          <Sparkline
            label="Active requests"
            display={String(stats?.activeRequests ?? 0)}
            values={history.map(item => item.activeRequests)}
            tone="info"
            yMax={Math.max(2, ...history.map(item => item.activeRequests))}
          />
        </div>
        <CombinedUsageChart
          range={usageRange}
          onRangeChange={setUsageRange}
          labels={llmSeries.months.length >= dictationSeries.months.length ? llmSeries.months : dictationSeries.months}
          llmRequests={alignSeries(llmSeries.months, llmSeries.requests, llmSeries.months.length >= dictationSeries.months.length ? llmSeries.months : dictationSeries.months)}
          dictationRequests={alignSeries(dictationSeries.months, dictationSeries.requests, llmSeries.months.length >= dictationSeries.months.length ? llmSeries.months : dictationSeries.months)}
        />
      </Panel>

      <Panel>
        <SectionTitle title="Recent activity" aside="all services · last 10" />
        <div className="mt-2 divide-y divide-white/10">
          {activity.length === 0 ? (
            <div className="py-4">
              <EmptyState title="No live activity yet" detail="Persisted totals above remain available across gateway restarts." />
            </div>
          ) : (
            activity.slice(0, 10).map(item => (
              <div key={item.id} className="flex min-w-0 items-center gap-3 py-2.5">
                <Badge
                  label={item.tone === 'critical' ? 'Failed' : item.tone === 'good' ? 'Completed' : item.tone === 'info' ? 'In progress' : 'Status'}
                  tone={item.tone}
                />
                <div className="min-w-0 flex-1">
                  <p className="truncate text-sm text-text-primary">{item.label}</p>
                  {item.detail && <p className={`truncate text-xs ${item.tone === 'critical' ? toneText('critical') : 'text-text-muted'}`}>{item.detail}</p>}
                </div>
                <span className="shrink-0 text-xs text-text-muted">{timeAgo(item.timestampUnixMs)}</span>
              </div>
            ))
          )}
        </div>
      </Panel>

      {swap.lastError && (
        <div className="border-l-2 border-warning-amber bg-warning-amber/10 px-4 py-2 text-sm text-warning-amber">
          Last model load failed: {swap.lastError}
        </div>
      )}
    </div>
  );
};

const ServiceSummary: React.FC<{
  title: string;
  href: string;
  registered: number;
  loaded: number;
  requests: number;
  usageLabel: string;
  usageValue: string;
  costValue: string;
  detail: string;
  dictation?: boolean;
}> = ({ title, href, registered, loaded, requests, usageLabel, usageValue, costValue, detail, dictation }) => (
  <Panel>
    <div className="flex items-start justify-between gap-3">
      <div>
        <h2 className="text-base font-semibold text-text-primary">{title}</h2>
        <p className="mt-1 text-xs text-text-muted">{detail}</p>
      </div>
      <Badge label={loaded ? `${loaded} loaded` : 'Standby'} tone={loaded ? 'good' : 'idle'} />
    </div>
    <div className="mt-4 grid grid-cols-2 gap-4 sm:grid-cols-4">
      <Stat label="Models" value={String(registered)} />
      <Stat label="Requests" value={requests.toLocaleString()} />
      <Stat label={usageLabel} value={usageValue} />
      <Stat label="API-equivalent value" value={costValue} tone="good" />
    </div>
    <a
      href={href}
      className={`mt-4 inline-flex rounded px-3 py-2 text-sm font-medium text-[#08111f] ${dictation ? 'bg-infer-violet' : 'bg-queue-blue'}`}
    >
      Open {title}
    </a>
  </Panel>
);

const CombinedUsageChart: React.FC<{
  range: TokenRange;
  onRangeChange: (range: TokenRange) => void;
  labels: string[];
  llmRequests: number[];
  dictationRequests: number[];
}> = ({ range, onRangeChange, labels, llmRequests, dictationRequests }) => {
  return (
    <div className="mt-5 border-t border-border-slate pt-4">
      <SectionTitle title="Combined usage" aside={`${TOKEN_RANGE_LABELS[range]} · requests by AI service`} />
      <div className="mt-3"><UsageRangeTabs value={range} onChange={onRangeChange} /></div>
      <UsageLineChart
        labels={labels}
        series={[
          { label: 'LLM', color: '#60A5FA', values: llmRequests },
          { label: 'Dictation', color: '#A78BFA', values: dictationRequests },
        ]}
        ariaLabel={`Combined AI service requests for ${TOKEN_RANGE_LABELS[range]}`}
      />
    </div>
  );
};

function alignSeries(sourceLabels: string[], values: number[], targetLabels: string[]): number[] {
  const byLabel = new Map(sourceLabels.map((label, index) => [label, values[index] ?? 0]));
  return targetLabels.map(label => byLabel.get(label) ?? 0);
}

function withSharedBuckets(rows: MonthlyUsageRow[], allRows: MonthlyUsageRow[]): MonthlyUsageRow[] {
  const present = new Set(rows.map(row => row.bucket));
  const axisRows = Array.from(new Set(allRows.map(row => row.bucket)))
    .filter(bucket => !present.has(bucket))
    .map(bucket => ({
      bucket,
      model: '__axis__',
      promptTokens: 0,
      completionTokens: 0,
      totalTokens: 0,
      requests: 0,
      successfulRequests: 0,
      inputAudioSeconds: 0,
      inputCharacters: 0,
    }));
  return [...rows, ...axisRows];
}

function tokenShare(part: number, total: number): string | undefined {
  if (!total) return undefined;
  return `${Math.round((part / total) * 100)}% of total`;
}
