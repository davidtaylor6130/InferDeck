import React, { useEffect, useMemo, useRef, useState } from 'react';
import { getJobs, getPricing } from '../api';
import { UsageRangeTabs } from '../components/UsageCharts';
import { Panel, SectionTitle, Stat, linePath, pickTickIndices } from '../components/ui';
import {
  ALL_MODELS,
  DEFAULT_COST_CONFIG,
  TOKEN_RANGE_LABELS,
  buildCostDefaults,
  buildTokenSeries,
  getCostConfigForModel,
  tokenUsageFromSeries,
} from '../cost';
import type { CostDefaults, ModelCostConfig, TokenRange, TokenSeries } from '../cost';
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

export const UsagePage: React.FC<{ section?: DashboardSection }> = ({ section = 'llm' }) => (
  section === 'dictation' ? <DictationUsagePage /> : <LlmUsagePage />
);

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

  return (
    <div className="space-y-4">
      <Panel>
        <SectionTitle title="LLM usage" aside={TOKEN_RANGE_LABELS[range]} />
        <div className="mt-3 grid grid-cols-2 gap-4 sm:grid-cols-4">
          <Stat label="Total tokens" value={formatTokenCount(seriesUsage.total)} />
          <Stat label="Prompt" value={formatTokenCount(seriesUsage.prompt)} />
          <Stat label="Output" value={formatTokenCount(seriesUsage.output)} />
          <Stat label="Estimated API cost" value={formatCurrency(rangeCost)} tone="good" />
        </div>

        <label className="mt-4 block max-w-sm text-xs text-text-secondary">
          <span className="mb-1 block text-text-muted">Usage model</span>
          <select
            className="h-9 w-full rounded-md border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary"
            value={selectedModel}
            onChange={event => setSelectedModel(event.target.value)}
          >
            {modelNames.map(model => <option key={model} value={model}>{model}</option>)}
          </select>
        </label>
        <div className="mt-3"><UsageRangeTabs value={range} onChange={setRange} /></div>

        {series.total.some(value => value > 0)
          ? <TokenUsageGraph series={series} />
          : <p className="border-y border-dashed border-border-slate py-8 text-center text-sm text-text-muted">No usage recorded for this range.</p>}

        <p className="mt-4 border-t border-border-slate pt-3 text-xs text-text-muted">
          Cost uses the server-side prices configured for each model in Model Settings. Models without prices contribute no estimated cost.
        </p>
      </Panel>

      <Panel>
        <SectionTitle title="Per-model usage" aside={TOKEN_RANGE_LABELS[range]} />
        <p className="mt-2 text-xs text-text-muted">
          TPS is average generation speed. Prompt processing is uncached input processing speed. Peak TPS is the fastest comparable generation request.
        </p>
        {periodUsage.length === 0 ? (
          <p className="mt-3 text-sm text-text-muted">No usage recorded for this range.</p>
        ) : (
          <div className="mt-3 overflow-x-auto">
            <table className="w-full min-w-[880px] text-left text-sm">
              <thead>
                <tr className="border-b border-white/10 text-xs uppercase tracking-wide text-text-muted">
                  <SortableHeader label="Model" sortKey="model" active={sort} onSort={toggleSort} />
                  <SortableHeader label="Requests" sortKey="requests" active={sort} onSort={toggleSort} />
                  <SortableHeader label="Prompt" sortKey="promptTokens" active={sort} onSort={toggleSort} />
                  <SortableHeader label="Output" sortKey="completionTokens" active={sort} onSort={toggleSort} />
                  <SortableHeader label="TPS" title="Duration-weighted average generation tokens per second" sortKey="avgTokensPerSecond" active={sort} onSort={toggleSort} />
                  <SortableHeader label="Prompt processing" title="Duration-weighted uncached prompt-processing tokens per second" sortKey="avgPromptTokensPerSecond" active={sort} onSort={toggleSort} />
                  <SortableHeader label="Peak TPS" title="Fastest comparable single-request generation speed" sortKey="peakTokensPerSecond" active={sort} onSort={toggleSort} />
                  <SortableHeader label="Cost" sortKey="cost" active={sort} onSort={toggleSort} last />
                </tr>
              </thead>
              <tbody className="divide-y divide-white/5">
                {periodUsage.map(row => {
                  return (
                    <tr key={row.model}>
                      <td className="py-2 pr-4 font-mono text-text-primary">{compactModel(row.model)}</td>
                      <td className="py-2 pr-4 text-text-secondary">{row.requests} <span className="text-text-muted">({row.successfulRequests} ok)</span></td>
                      <td className="py-2 pr-4 text-text-secondary">{formatTokenCount(row.promptTokens)}</td>
                      <td className="py-2 pr-4 text-text-secondary">{formatTokenCount(row.completionTokens)}</td>
                      <td className="py-2 pr-4 text-text-secondary">{row.avgTokensPerSecond ? row.avgTokensPerSecond.toFixed(1) : '—'}</td>
                      <td className="py-2 pr-4 text-text-secondary">{row.avgPromptTokensPerSecond ? row.avgPromptTokensPerSecond.toFixed(1) : '—'}</td>
                      <td className="py-2 pr-4 text-text-secondary">{row.peakTokensPerSecond ? row.peakTokensPerSecond.toFixed(1) : '—'}</td>
                      <td className="py-2 text-success-green">{formatCurrency(row.cost)}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </Panel>
    </div>
  );
};

const TokenUsageGraph: React.FC<{ series: TokenSeries }> = ({ series }) => {
  const tokenMax = Math.max(1, ...series.total, ...series.prompt, ...series.output);
  const costMax = Math.max(1, ...series.cost);
  const lastIndex = series.months.length - 1;
  const monthX = (index: number) => lastIndex <= 0 ? 340 : (680 / lastIndex) * index;
  const pointY = (value: number, max: number) => 150 - (clamp(value, 0, max) / max) * 150;
  const tickIndices = pickTickIndices(series.months.length);
  const svgRef = useRef<SVGSVGElement>(null);
  const [hoverIndex, setHoverIndex] = useState<number | null>(null);

  const handlePointerMove = (event: React.MouseEvent<SVGSVGElement>) => {
    const rect = svgRef.current?.getBoundingClientRect();
    if (!rect || series.months.length === 0) return;
    const ratio = clamp((event.clientX - rect.left) / rect.width, 0, 1);
    setHoverIndex(lastIndex <= 0 ? 0 : Math.round(ratio * lastIndex));
  };
  const handlePointerLeave = () => setHoverIndex(null);
  // -50% centers the label/tooltip on the point; edges anchor inward so they don't overflow the chart.
  const edgeTranslateX = (index: number) => index === 0 ? '0%' : index === lastIndex ? '-100%' : '-50%';

  return (
    <div className="mt-5">
      <div className="grid grid-cols-[34px_1fr_42px] gap-2 text-xs text-text-muted">
        <div className="flex flex-col justify-between py-2"><span>{formatTokenCount(tokenMax)}</span><span>{formatTokenCount(tokenMax / 2)}</span><span>0</span></div>
        <div>
          <div className="relative">
            <svg
              ref={svgRef}
              viewBox="0 0 680 150"
              preserveAspectRatio="none"
              className="h-[150px] w-full overflow-visible"
              role="img"
              aria-label="Token usage over time. A data table follows the chart."
              onMouseMove={handlePointerMove}
              onMouseLeave={handlePointerLeave}
            >
              <g stroke="rgba(148,163,184,0.14)" strokeDasharray="4 5" vectorEffect="non-scaling-stroke">
                {[0, 75, 150].map(y => <line key={y} x1="0" y1={y} x2="680" y2={y} vectorEffect="non-scaling-stroke" />)}
                {series.months.map((_, index) => <line key={index} x1={monthX(index)} y1="0" x2={monthX(index)} y2="150" vectorEffect="non-scaling-stroke" />)}
              </g>
              <path d={linePath(series.total, 680, 150, tokenMax)} fill="none" stroke="#60A5FA" strokeWidth="3" vectorEffect="non-scaling-stroke" />
              <path d={linePath(series.prompt, 680, 150, tokenMax)} fill="none" stroke="#34D399" strokeWidth="3" vectorEffect="non-scaling-stroke" />
              <path d={linePath(series.output, 680, 150, tokenMax)} fill="none" stroke="#A78BFA" strokeWidth="3" vectorEffect="non-scaling-stroke" />
              <path d={linePath(series.cost, 680, 150, costMax)} fill="none" stroke="#22C55E" strokeWidth="3" strokeDasharray="8 6" vectorEffect="non-scaling-stroke" />
              {hoverIndex !== null && (
                <g pointerEvents="none">
                  <line x1={monthX(hoverIndex)} y1="0" x2={monthX(hoverIndex)} y2="150" stroke="rgba(226,232,240,0.35)" strokeWidth="1" vectorEffect="non-scaling-stroke" />
                  <circle cx={monthX(hoverIndex)} cy={pointY(series.total[hoverIndex], tokenMax)} r="3.5" fill="#60A5FA" />
                  <circle cx={monthX(hoverIndex)} cy={pointY(series.prompt[hoverIndex], tokenMax)} r="3.5" fill="#34D399" />
                  <circle cx={monthX(hoverIndex)} cy={pointY(series.output[hoverIndex], tokenMax)} r="3.5" fill="#A78BFA" />
                </g>
              )}
            </svg>
            {hoverIndex !== null && (
              <div
                className="pointer-events-none absolute z-10 whitespace-nowrap rounded-md border border-white/10 bg-[#0b1626] px-2.5 py-1.5 text-xs shadow-deck"
                style={{
                  left: `${(monthX(hoverIndex) / 680) * 100}%`,
                  top: `${(pointY(series.total[hoverIndex], tokenMax) / 150) * 100}%`,
                  transform: `translate(${edgeTranslateX(hoverIndex)}, calc(-100% - 10px))`,
                }}
              >
                <div className="font-medium text-text-primary">{series.months[hoverIndex] || 'Bucket'}</div>
                <div className="mt-1 space-y-0.5 text-text-secondary">
                  <div>Total: <span className="text-text-primary">{formatTokenCount(series.total[hoverIndex])}</span></div>
                  <div>Prompt: <span className="text-text-primary">{formatTokenCount(series.prompt[hoverIndex])}</span></div>
                  <div>Output: <span className="text-text-primary">{formatTokenCount(series.output[hoverIndex])}</span></div>
                  <div>Cost: <span className="text-success-green">{formatCurrency(series.cost[hoverIndex])}</span></div>
                </div>
              </div>
            )}
          </div>
          <div className="relative mt-1 h-4 text-xs text-text-muted">
            {series.months.map((month, index) => tickIndices.includes(index) && (
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
        <div className="flex flex-col justify-between py-2 text-right"><span>{formatCurrency(costMax)}</span><span>{formatCurrency(costMax / 2)}</span><span>$0</span></div>
      </div>
      <div className="mt-4 flex flex-wrap gap-x-5 gap-y-2 text-xs text-text-secondary">
        <Legend color="#60A5FA" label="Total Tokens" />
        <Legend color="#34D399" label="Prompt Tokens" />
        <Legend color="#A78BFA" label="Output Tokens" />
        <Legend color="#22C55E" label="Estimated API cost (USD)" dashed />
      </div>
      <table className="sr-only">
        <caption>Token usage chart values</caption>
        <thead><tr><th>Period</th><th>Total tokens</th><th>Prompt tokens</th><th>Output tokens</th><th>Estimated API cost</th></tr></thead>
        <tbody>
          {series.months.map((month, index) => (
            <tr key={month}>
              <th>{month}</th>
              <td>{series.total[index]}</td>
              <td>{series.prompt[index]}</td>
              <td>{series.output[index]}</td>
              <td>{series.cost[index]}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
};

const Legend: React.FC<{ color: string; label: string; dashed?: boolean }> = ({ color, label, dashed }) => (
  <span className="inline-flex items-center gap-2">
    <span className={`w-6 ${dashed ? 'border-t-2 border-dashed' : 'h-0.5'}`} style={dashed ? { borderColor: color } : { background: color }} />
    {label}
  </span>
);

const SortableHeader: React.FC<{
  label: string;
  sortKey: UsageSortKey;
  active: { key: UsageSortKey; direction: 'asc' | 'desc' };
  onSort: (key: UsageSortKey) => void;
  title?: string;
  last?: boolean;
}> = ({ label, sortKey, active, onSort, title, last }) => {
  const selected = active.key === sortKey;
  const ariaSort = selected ? (active.direction === 'asc' ? 'ascending' : 'descending') : 'none';
  return (
    <th className={`py-2 font-medium ${last ? '' : 'pr-4'}`} aria-sort={ariaSort}>
      <button
        type="button"
        title={title}
        onClick={() => onSort(sortKey)}
        className="rounded text-left hover:text-text-primary focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-queue-blue"
      >
        {label}{selected ? (active.direction === 'asc' ? ' ↑' : ' ↓') : ''}
      </button>
    </th>
  );
};
