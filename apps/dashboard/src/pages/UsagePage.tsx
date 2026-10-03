import React, { useEffect, useMemo, useRef, useState } from 'react';
import { getJobs, getPricing } from '../api';
import { UsageRangeTabs } from '../components/UsageCharts';
import { Button, DetailItem, EmptyState, SectionTitle, linePath, pickTickIndices } from '../components/ui';
import { modelHref } from '../routes';
import {
  ALL_MODELS,
  DEFAULT_COST_CONFIG,
  TOKEN_RANGE_LABELS,
  buildCostDefaults,
  buildTokenSeries,
  getCostConfigForModel,
  tokenUsageFromSeries,
} from '../cost';
import type { CostDefaults, ModelCostConfig, TokenRange } from '../cost';
import {
  bucketUsageForSection,
  isDictationModel,
  modelsForSection,
  usageForSection,
  type DashboardSection,
} from '../dashboardSections';
import { useGateway } from '../gateway';
import type { JobRecord, UsageRow } from '../types';
import { clamp, compactModel, formatCurrency, formatTokenCount } from '../utils';
import { DictationUsagePage } from './DictationUsagePage';
import { SubscriptionSavingsPanel } from '../components/SubscriptionSavingsPanel';
import { MediaGenerationUsagePage } from './MediaGenerationUsagePage';

export const UsagePage: React.FC<{ section?: DashboardSection }> = ({ section = 'llm' }) => {
  if (section === 'dictation') return <DictationUsagePage />;
  if (section === 'image' || section === 'music' || section === 'video') {
    return <MediaGenerationUsagePage section={section} />;
  }
  return <LlmUsagePage />;
};

const RANGE_PHRASE: Record<TokenRange, string> = {
  day: 'in the last 24 hours',
  week: 'in the last week',
  month: 'in the last month',
  year: 'in the last year',
  all: 'in total',
};

type UsageSortKey = 'model' | 'requests' | 'promptTokens' | 'completionTokens' | 'avgTokensPerSecond' | 'avgPromptTokensPerSecond' | 'peakTokensPerSecond' | 'cost';
const sum = (values: number[]) => values.reduce((total, value) => total + value, 0);
const measuredRate = (tokens: number, durationMs: number, peak: number) => {
  if (tokens <= 0 || durationMs <= 0 || peak <= 0) return 0;
  return Math.min(tokens / (durationMs / 1000), peak);
};

const LlmUsagePage: React.FC = () => {
  const { status, models } = useGateway();
  const [jobs, setJobs] = useState<JobRecord[]>([]);
  const [defaults, setDefaults] = useState<{ defaults: CostDefaults; fallback: ModelCostConfig }>({ defaults: {}, fallback: DEFAULT_COST_CONFIG });
  const [selectedModel, setSelectedModel] = useState(ALL_MODELS);
  const [range, setRange] = useState<TokenRange>('all');
  const [sort, setSort] = useState<{ key: UsageSortKey; direction: 'asc' | 'desc' }>({ key: 'model', direction: 'asc' });

  useEffect(() => {
    let active = true;
    getJobs(200).then(rows => { if (active) setJobs(rows); }).catch(() => {});
    getPricing().then(pricing => {
      if (!active) return;
      const built = buildCostDefaults(pricing);
      setDefaults(built);
    }).catch(() => {});
    return () => { active = false; };
  }, []);

  const usage = useMemo(
    () => usageForSection(status?.tokenUsage ?? [], models, 'llm'),
    [status?.tokenUsage, models],
  );
  const monthly = useMemo(
    () => bucketUsageForSection(status?.monthlyTokenUsage ?? [], models, 'llm'),
    [status?.monthlyTokenUsage, models],
  );
  const daily = useMemo(
    () => bucketUsageForSection(status?.dailyTokenUsage ?? [], models, 'llm'),
    [status?.dailyTokenUsage, models],
  );
  const hourly = useMemo(
    () => bucketUsageForSection(status?.hourlyTokenUsage ?? [], models, 'llm'),
    [status?.hourlyTokenUsage, models],
  );
  const llmModels = useMemo(() => modelsForSection(models, 'llm'), [models]);
  const dictationIds = useMemo(
    () => new Set(models.filter(isDictationModel).map(model => model.id)),
    [models],
  );
  const llmJobs = useMemo(() => jobs.filter(job => !dictationIds.has(job.model)), [jobs, dictationIds]);

  const modelNames = useMemo(() => {
    const names = new Set<string>([ALL_MODELS]);
    for (const row of usage) names.add(row.model);
    for (const model of llmModels) names.add(model.id);
    return Array.from(names);
  }, [usage, llmModels]);

  useEffect(() => {
    if (!modelNames.includes(selectedModel)) setSelectedModel(ALL_MODELS);
  }, [modelNames, selectedModel]);

  const pricingByModel: Record<string, ModelCostConfig> = {};
  const selectedCost = getCostConfigForModel(selectedModel, pricingByModel, defaults.defaults, defaults.fallback);
  const series = useMemo(
    () => buildTokenSeries(llmJobs, selectedModel, selectedCost, monthly, pricingByModel, defaults.defaults, defaults.fallback, range, daily, hourly, Boolean(status?.dailyTokenUsageAllTime)),
    [llmJobs, selectedModel, selectedCost, monthly, defaults, range, daily, hourly, status?.dailyTokenUsageAllTime],
  );
  const seriesUsage = useMemo(() => tokenUsageFromSeries(selectedModel, series), [selectedModel, series]);
  const rangeCost = series.cost.reduce((sum, value) => sum + value, 0);
  const allTimeSeries = useMemo(
    () => buildTokenSeries(llmJobs, ALL_MODELS, DEFAULT_COST_CONFIG, monthly, pricingByModel, defaults.defaults, defaults.fallback, 'all', daily, hourly, Boolean(status?.dailyTokenUsageAllTime)),
    [llmJobs, monthly, daily, hourly, defaults, status?.dailyTokenUsageAllTime],
  );
  const equivalentApiCostCents = Math.round(allTimeSeries.cost.reduce((total, value) => total + value, 0) * 100);

  const periodUsage = useMemo(() => {
    const rows = modelNames.filter(model => model !== ALL_MODELS).map((model, index) => {
      const cost = getCostConfigForModel(model, pricingByModel, defaults.defaults, defaults.fallback);
      const modelSeries = buildTokenSeries(
        llmJobs, model, cost, monthly, pricingByModel, defaults.defaults,
        defaults.fallback, range, daily, hourly, Boolean(status?.dailyTokenUsageAllTime),
      );
      const promptTokens = sum(modelSeries.prompt);
      const cachedPromptTokens = sum(modelSeries.cachedPrompt);
      const completionTokens = sum(modelSeries.output);
      const requests = sum(modelSeries.requests);
      const generationDurationMs = sum(modelSeries.generationDurationMs);
      const promptDurationMs = sum(modelSeries.promptDurationMs);
      const measuredCompletionTokens = sum(modelSeries.measuredCompletionTokens);
      const measuredPromptTokens = sum(modelSeries.measuredPromptTokens);
      const peakTokensPerSecond = Math.max(0, ...modelSeries.peakTokensPerSecond);
      const peakPromptTokensPerSecond = Math.max(0, ...modelSeries.peakPromptTokensPerSecond);
      return {
        index,
        model,
        requests,
        successfulRequests: sum(modelSeries.successfulRequests),
        promptTokens,
        cachedPromptTokens,
        completionTokens,
        totalTokens: promptTokens + completionTokens,
        avgTokensPerSecond: measuredRate(measuredCompletionTokens, generationDurationMs, peakTokensPerSecond),
        peakTokensPerSecond,
        avgPromptTokensPerSecond: measuredRate(measuredPromptTokens, promptDurationMs, peakPromptTokensPerSecond),
        peakPromptTokensPerSecond,
        lastTimestampUnixMs: usage.find(row => row.model === model)?.lastTimestampUnixMs ?? 0,
        cost: modelSeries.cost.reduce((total, value) => total + value, 0),
      } satisfies UsageRow & { index: number; cost: number };
    }).filter(row => row.requests > 0 || row.totalTokens > 0);
    const direction = sort.direction === 'asc' ? 1 : -1;
    return rows.sort((left, right) => {
      const a = left[sort.key];
      const b = right[sort.key];
      const compared = typeof a === 'string'
        ? a.localeCompare(String(b))
        : Number(a ?? Number.NEGATIVE_INFINITY) - Number(b ?? Number.NEGATIVE_INFINITY);
      return compared === 0 ? left.index - right.index : compared * direction;
    });
  }, [modelNames, defaults, llmJobs, monthly, range, daily, hourly, status?.dailyTokenUsageAllTime, usage, sort]);

  const toggleSort = (key: UsageSortKey) => {
    setSort(current => current.key === key
      ? { key, direction: current.direction === 'asc' ? 'desc' : 'asc' }
      : { key, direction: key === 'model' ? 'asc' : 'desc' });
  };

  const sortLabels: Record<UsageSortKey, string> = {
    model: 'Model',
    requests: 'Requests',
    promptTokens: 'Prompt',
    completionTokens: 'Output',
    avgTokensPerSecond: 'TPS',
    avgPromptTokensPerSecond: 'Prompt processing',
    peakTokensPerSecond: 'Peak TPS',
    cost: 'Cost',
  };
  const maxTokens = Math.max(1, ...periodUsage.map(row => row.totalTokens));
  const busiest = periodUsage.reduce<(typeof periodUsage)[number] | undefined>(
    (top, row) => (!top || row.totalTokens > top.totalTokens ? row : top), undefined);

  return (
    <div className="space-y-8">
      <header aria-label={`Summary for ${TOKEN_RANGE_LABELS[range]}`}>
        <div className="flex flex-wrap items-center justify-between gap-3">
          <p className="text-sm text-text-muted">LLM usage</p>
          <UsageRangeTabs value={range} onChange={setRange} />
        </div>
        <h1 className="mt-2 text-2xl font-semibold text-text-primary sm:text-[28px] sm:leading-9">
          {formatTokenCount(seriesUsage.total)} tokens {RANGE_PHRASE[range]}
        </h1>
        <p className="mt-1 max-w-[70ch] text-sm text-text-secondary">
          Worth about <span className="font-medium text-text-primary">{formatCurrency(rangeCost)}</span> at hosted API prices.
          {' '}{formatTokenCount(seriesUsage.prompt)} read, {formatTokenCount(seriesUsage.output)} written.
          {busiest ? <> Busiest model: <a href={modelHref(busiest.model)} className="font-mono text-text-primary hover:underline">{compactModel(busiest.model)}</a> ({Math.round(busiest.totalTokens / Math.max(1, seriesUsage.total) * 100)}%).</> : null}
        </p>
      </header>

      <section>
        <SectionTitle
          title="Tokens over time"
          action={(
            <label className="flex items-center gap-2 text-xs text-text-muted">
              <span className="sr-only">Usage model</span>
              <select
                aria-label="Usage model"
                className="h-8 max-w-[240px] px-2.5 font-mono text-xs"
                value={selectedModel}
                onChange={event => setSelectedModel(event.target.value)}
              >
                {modelNames.map(model => <option key={model} value={model}>{model}</option>)}
              </select>
            </label>
          )}
        />
        {series.total.some(value => value > 0) ? (
          <div className="grid gap-8 lg:grid-cols-[minmax(0,1.6fr)_minmax(0,1fr)]">
            <LineGraph
              labels={series.months}
              ariaLabel="Prompt and output tokens over time"
              format={formatTokenCount}
              lines={[
                { label: 'Prompt tokens', color: 'rgb(var(--series-1))', values: series.prompt },
                { label: 'Output tokens', color: 'rgb(var(--series-2))', values: series.output },
              ]}
            />
            <div>
              <h3 className="mt-4 text-xs text-text-muted">Estimated API cost (USD)</h3>
              <LineGraph
                labels={series.months}
                ariaLabel="Estimated API cost over time"
                format={formatCurrency}
                maxTicks={2}
                lines={[{ label: 'Estimated API cost', color: 'rgb(var(--series-1))', values: series.cost }]}
              />
            </div>
          </div>
        ) : <div className="mt-4"><EmptyState title="No usage recorded for this range." /></div>}
        <p className="mt-4 max-w-[80ch] text-xs text-text-muted">
          Cost uses the server-side prices configured for each model in Model Settings. Legacy ranges may use a server-provided cache estimate where early versions did not record cache hits. Models without prices contribute no estimated cost.
        </p>
      </section>

      <section>
        <div className="flex flex-wrap items-end justify-between gap-3 pb-2">
          <h2 className="text-sm font-semibold">Per-model usage <span className="font-normal text-text-muted">{TOKEN_RANGE_LABELS[range]}</span></h2>
          <div className="flex items-center gap-2 md:hidden">
            <select
              aria-label="Sort mobile LLM usage"
              className="h-9 px-2.5 text-sm"
              value={sort.key}
              onChange={event => toggleSort(event.target.value as UsageSortKey)}
            >
              {(Object.keys(sortLabels) as UsageSortKey[]).map(key => <option key={key} value={key}>{sortLabels[key]}</option>)}
            </select>
            <Button onClick={() => setSort(current => ({ ...current, direction: current.direction === 'asc' ? 'desc' : 'asc' }))}>
              {sort.direction === 'asc' ? 'Ascending' : 'Descending'}
            </Button>
          </div>
        </div>
        {periodUsage.length === 0 ? (
          <EmptyState title="No usage recorded for this range." />
        ) : (
          <>
          <div className="divide-y divide-border-slate rounded-lg border border-border-slate bg-panel-slate shadow-card md:hidden" aria-label="Per-model LLM usage cards">
            {periodUsage.map(row => (
              <article key={row.model} className="px-3 py-3">
                <div className="flex items-center gap-3">
                  <h3 className="min-w-0 flex-1 truncate font-mono text-sm text-text-primary">{compactModel(row.model)}</h3>
                  <span className="tabular text-sm font-medium text-text-primary">{formatCurrency(row.cost)}</span>
                </div>
                <dl className="mt-3 grid grid-cols-3 gap-x-4 gap-y-3">
                  <DetailItem label="Requests">
                    {row.requests.toLocaleString()} <span className="text-xs text-text-muted">({row.successfulRequests.toLocaleString()} ok)</span>
                  </DetailItem>
                  <DetailItem label="Prompt">{formatTokenCount(row.promptTokens)}</DetailItem>
                  <DetailItem label="Output">{formatTokenCount(row.completionTokens)}</DetailItem>
                  <DetailItem label="TPS">{row.avgTokensPerSecond ? row.avgTokensPerSecond.toFixed(1) : '—'}</DetailItem>
                  <DetailItem label="Prompt processing">{row.avgPromptTokensPerSecond ? row.avgPromptTokensPerSecond.toFixed(1) : '—'}</DetailItem>
                  <DetailItem label="Peak TPS">{row.peakTokensPerSecond ? row.peakTokensPerSecond.toFixed(1) : '—'}</DetailItem>
                </dl>
              </article>
            ))}
          </div>
          <div className="hidden overflow-x-auto rounded-lg border border-border-slate bg-panel-slate shadow-card md:block" role="region" aria-label="Per-model LLM usage" tabIndex={0}>
            <table className="w-full min-w-[880px] text-left text-sm">
              <thead>
                <tr className="text-xs text-text-muted">
                  <SortableHeader label="Model" sortKey="model" active={sort} onSort={toggleSort} first />
                  <SortableHeader label="Requests" sortKey="requests" active={sort} onSort={toggleSort} />
                  <SortableHeader label="Prompt" sortKey="promptTokens" active={sort} onSort={toggleSort} />
                  <SortableHeader label="Output" sortKey="completionTokens" active={sort} onSort={toggleSort} />
                  <SortableHeader label="TPS" title="Duration-weighted average generation tokens per second" sortKey="avgTokensPerSecond" active={sort} onSort={toggleSort} />
                  <SortableHeader label="Prompt processing" title="Duration-weighted uncached prompt-processing tokens per second" sortKey="avgPromptTokensPerSecond" active={sort} onSort={toggleSort} />
                  <SortableHeader label="Peak TPS" title="Fastest comparable single-request generation speed" sortKey="peakTokensPerSecond" active={sort} onSort={toggleSort} />
                  <SortableHeader label="Cost" sortKey="cost" active={sort} onSort={toggleSort} last />
                </tr>
              </thead>
              <tbody className="tabular">
                {periodUsage.map(row => (
                  <tr key={row.model} className="border-t border-border-slate hover:bg-elevated-slate/60">
                    <td className="py-2 pl-3 pr-4">
                      <p className="truncate font-mono text-text-primary" title={row.model}>{compactModel(row.model)}</p>
                      <div className="mt-1 h-1 w-28 overflow-hidden rounded-sm bg-border-slate"><div className="h-full bg-series-1" style={{ width: `${row.totalTokens / maxTokens * 100}%` }} /></div>
                    </td>
                    <td className="py-2 pr-4 text-right text-text-secondary">{row.requests.toLocaleString()} <span className="text-text-muted">({row.successfulRequests} ok)</span></td>
                    <td className="py-2 pr-4 text-right text-text-secondary">{formatTokenCount(row.promptTokens)}</td>
                    <td className="py-2 pr-4 text-right text-text-secondary">{formatTokenCount(row.completionTokens)}</td>
                    <td className="py-2 pr-4 text-right text-text-primary">{row.avgTokensPerSecond ? row.avgTokensPerSecond.toFixed(1) : '—'}</td>
                    <td className="py-2 pr-4 text-right text-text-secondary">{row.avgPromptTokensPerSecond ? row.avgPromptTokensPerSecond.toFixed(1) : '—'}</td>
                    <td className="py-2 pr-4 text-right text-text-secondary">{row.peakTokensPerSecond ? row.peakTokensPerSecond.toFixed(1) : '—'}</td>
                    <td className="py-2 pr-3 text-right font-medium text-text-primary">{formatCurrency(row.cost)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          </>
        )}
        <p className="mt-2 text-xs text-text-muted">
          TPS is average generation speed. Prompt processing is uncached input processing speed. Peak TPS is the fastest comparable generation request.
        </p>
      </section>

      <SubscriptionSavingsPanel apiCostsCents={equivalentApiCostCents} />
    </div>
  );
};


type GraphLine = { label: string; color: string; values: number[] };

const LineGraph: React.FC<{ labels: string[]; lines: GraphLine[]; format: (value: number) => string; ariaLabel: string; maxTicks?: number }> = ({ labels, lines, format, ariaLabel, maxTicks = 4 }) => {
  const max = Math.max(1, ...lines.flatMap(line => line.values));
  const lastIndex = labels.length - 1;
  const monthX = (index: number) => lastIndex <= 0 ? 340 : (680 / lastIndex) * index;
  const pointY = (value: number) => 150 - (clamp(value, 0, max) / max) * 150;
  const tickIndices = pickTickIndices(labels.length, maxTicks);
  const svgRef = useRef<SVGSVGElement>(null);
  const [hoverIndex, setHoverIndex] = useState<number | null>(null);

  const handlePointerMove = (event: React.PointerEvent<SVGSVGElement>) => {
    const rect = svgRef.current?.getBoundingClientRect();
    if (!rect || labels.length === 0) return;
    const ratio = clamp((event.clientX - rect.left) / rect.width, 0, 1);
    setHoverIndex(lastIndex <= 0 ? 0 : Math.round(ratio * lastIndex));
  };
  // -50% centers the label/tooltip on the point; edges anchor inward so they don't overflow the chart.
  const edgeTranslateX = (index: number) => index === 0 ? '0%' : index === lastIndex ? '-100%' : '-50%';
  const topValue = hoverIndex === null ? 0 : Math.max(...lines.map(line => line.values[hoverIndex] ?? 0));

  return (
    <div className="mt-4">
      {lines.length > 1 && (
        <div className="mb-3 flex flex-wrap gap-x-5 gap-y-2 text-xs text-text-secondary">
          {lines.map(line => (
            <span key={line.label} className="inline-flex items-center gap-2">
              <span className="h-0.5 w-4" style={{ background: line.color }} />
              {line.label}
            </span>
          ))}
        </div>
      )}
      <div className="grid grid-cols-[44px_1fr] gap-2 text-xs text-text-muted">
        <div className="tabular flex flex-col justify-between text-right"><span>{format(max)}</span><span>{format(max / 2)}</span><span>0</span></div>
        <div>
          <div className="relative">
            <svg
              ref={svgRef}
              viewBox="0 0 680 150"
              preserveAspectRatio="none"
              className="chart-reveal h-[150px] w-full touch-pan-y overflow-visible"
              role="img"
              aria-label={`${ariaLabel}. A data table follows the chart.`}
              onPointerDown={handlePointerMove}
              onPointerMove={handlePointerMove}
              onPointerLeave={() => setHoverIndex(null)}
            >
              <g style={{ stroke: 'rgb(var(--separator))' }}>
                {[0, 75, 150].map(y => <line key={y} x1="0" y1={y} x2="680" y2={y} vectorEffect="non-scaling-stroke" />)}
              </g>
              {lines.map(line => (
                <path key={line.label} d={linePath(line.values, 680, 150, max)} fill="none" style={{ stroke: line.color }} strokeWidth="2" strokeLinejoin="round" vectorEffect="non-scaling-stroke" />
              ))}
              {hoverIndex !== null && (
                <line x1={monthX(hoverIndex)} y1="0" x2={monthX(hoverIndex)} y2="150" style={{ stroke: 'rgb(var(--line))' }} strokeWidth="1" vectorEffect="non-scaling-stroke" pointerEvents="none" />
              )}
            </svg>
            {hoverIndex !== null && lines.map(line => (
              <span
                key={line.label}
                className="pointer-events-none absolute h-2 w-2 -translate-x-1/2 -translate-y-1/2 rounded-full ring-2 ring-black"
                style={{ left: `${(monthX(hoverIndex) / 680) * 100}%`, top: `${(pointY(line.values[hoverIndex] ?? 0) / 150) * 100}%`, background: line.color }}
              />
            ))}
            {hoverIndex !== null && (
              <div
                className="pointer-events-none absolute z-10 max-w-[min(18rem,calc(100vw-2rem))] rounded-md border border-line-strong bg-panel-slate px-2.5 py-1.5 text-xs shadow-deck"
                style={{
                  left: `${(monthX(hoverIndex) / 680) * 100}%`,
                  top: `${(pointY(topValue) / 150) * 100}%`,
                  transform: `translate(${edgeTranslateX(hoverIndex)}, calc(-100% - 10px))`,
                }}
              >
                <div className="font-medium text-text-primary">{labels[hoverIndex] || 'Bucket'}</div>
                {lines.map(line => (
                  <div key={line.label} className="mt-0.5 flex items-center gap-1.5 text-text-secondary">
                    <span className="h-2 w-2 rounded-full" style={{ background: line.color }} />
                    {line.label}: <span className="tabular text-text-primary">{format(line.values[hoverIndex] ?? 0)}</span>
                  </div>
                ))}
              </div>
            )}
          </div>
          <div className="chart-ticks relative mt-1 h-4 text-xs text-text-muted">
            {labels.map((month, index) => tickIndices.includes(index) && (
              <span
                key={index}
                className="absolute whitespace-nowrap"
                style={{ left: `${lastIndex <= 0 ? 50 : (index / lastIndex) * 100}%`, transform: `translateX(${edgeTranslateX(index)})` }}
              >
                {month}
              </span>
            ))}
          </div>
        </div>
      </div>
      <table className="sr-only">
        <caption>{ariaLabel}</caption>
        <thead><tr><th>Period</th>{lines.map(line => <th key={line.label}>{line.label}</th>)}</tr></thead>
        <tbody>
          {labels.map((month, index) => (
            <tr key={month}>
              <th>{month}</th>
              {lines.map(line => <td key={line.label}>{line.values[index]}</td>)}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
};

const SortableHeader: React.FC<{
  label: string;
  sortKey: UsageSortKey;
  active: { key: UsageSortKey; direction: 'asc' | 'desc' };
  onSort: (key: UsageSortKey) => void;
  title?: string;
  last?: boolean;
  first?: boolean;
}> = ({ label, sortKey, active, onSort, title, last, first }) => {
  const selected = active.key === sortKey;
  const ariaSort = selected ? (active.direction === 'asc' ? 'ascending' : 'descending') : 'none';
  return (
    <th className={`py-2 font-medium ${first ? 'pl-3 pr-4 text-left' : last ? 'pr-3 text-right' : 'pr-4 text-right'}`} aria-sort={ariaSort}>
      <button
        type="button"
        title={title}
        onClick={() => onSort(sortKey)}
        className={`inline-flex items-center gap-1 rounded text-left transition-colors hover:text-text-primary ${selected ? 'text-text-primary' : ''}`}
      >
        {label}{selected ? <span className="text-queue-blue">{active.direction === 'asc' ? '↑' : '↓'}</span> : ''}
      </button>
    </th>
  );
};
