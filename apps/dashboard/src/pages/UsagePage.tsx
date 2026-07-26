import React, { useEffect, useMemo, useRef, useState } from 'react';
import { getJobs, getPricing } from '../api';
import { UsageRangeTabs } from '../components/UsageCharts';
import { Panel, SectionTitle, Stat, linePath, pickTickIndices } from '../components/ui';
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
import type { JobRecord } from '../types';
import { clamp, compactModel, formatCurrency, formatTokenCount } from '../utils';
import { DictationUsagePage } from './DictationUsagePage';

export const UsagePage: React.FC<{ section?: DashboardSection }> = ({ section = 'llm' }) => (
  section === 'dictation' ? <DictationUsagePage /> : <LlmUsagePage />
);

const LlmUsagePage: React.FC = () => {
  const { status, models } = useGateway();
  const [jobs, setJobs] = useState<JobRecord[]>([]);
  const [defaults, setDefaults] = useState<{ defaults: CostDefaults; fallback: ModelCostConfig }>({ defaults: {}, fallback: DEFAULT_COST_CONFIG });
  const [saved, setSaved] = useState<Record<string, ModelCostConfig>>({});
  const [selectedModel, setSelectedModel] = useState(ALL_MODELS);
  const [range, setRange] = useState<TokenRange>('all');

  useEffect(() => {
    let active = true;
    getJobs(200).then(rows => { if (active) setJobs(rows); }).catch(() => {});
    getPricing().then(pricing => {
      if (!active) return;
      const built = buildCostDefaults(pricing);
      setDefaults(built);
      setSaved(loadCostConfig(built.defaults, built.fallback));
    }).catch(() => {
      if (active) setSaved(loadCostConfig({}, DEFAULT_COST_CONFIG));
    });
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

  const selectedCost = getCostConfigForModel(selectedModel, saved, defaults.defaults, defaults.fallback);
  const series = useMemo(
    () => buildTokenSeries(llmJobs, selectedModel, selectedCost, monthly, saved, defaults.defaults, defaults.fallback, range, daily, hourly, Boolean(status?.dailyTokenUsageAllTime)),
    [llmJobs, selectedModel, selectedCost, monthly, saved, defaults, range, daily, hourly, status?.dailyTokenUsageAllTime],
  );
  const seriesUsage = useMemo(() => tokenUsageFromSeries(selectedModel, series), [selectedModel, series]);
  const rangeCost = series.cost.reduce((sum, value) => sum + value, 0);

  const persistConfig = (model: string, next: ModelCostConfig) => {
    const merged = {
      ...saved,
      [model]: { ...next, defaultsVersion: MODEL_COST_DEFAULTS_VERSION, userEdited: model !== ALL_MODELS },
    };
    setSaved(merged);
    saveCostConfig(merged);
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

        <details className="mt-4 border-t border-border-slate pt-3">
          <summary className="cursor-pointer text-sm font-medium text-text-secondary hover:text-text-primary">Cost assumptions</summary>
          <div className="mt-3 grid gap-3 lg:grid-cols-2">
          <label className={`min-w-0 text-xs text-text-secondary ${selectedModel === ALL_MODELS ? 'opacity-50' : ''}`}>
            <span className="mb-1 block text-text-muted">Prompt $/1M</span>
            <input
              className="h-9 w-full rounded-md border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary"
              type="number" min="0" step="0.01"
              value={selectedCost.promptPerMillion}
              disabled={selectedModel === ALL_MODELS}
              onChange={event => persistConfig(selectedModel, { ...selectedCost, promptPerMillion: Number(event.target.value) || 0 })}
            />
          </label>
          <label className={`min-w-0 text-xs text-text-secondary ${selectedModel === ALL_MODELS ? 'opacity-50' : ''}`}>
            <span className="mb-1 block text-text-muted">Output $/1M</span>
            <input
              className="h-9 w-full rounded-md border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary"
              type="number" min="0" step="0.01"
              value={selectedCost.outputPerMillion}
              disabled={selectedModel === ALL_MODELS}
              onChange={event => persistConfig(selectedModel, { ...selectedCost, outputPerMillion: Number(event.target.value) || 0 })}
            />
          </label>
          <p className="text-xs text-text-muted lg:col-span-2">
            {selectedModel === ALL_MODELS
              ? 'Estimated API cost applies saved per-model comparison prices to persisted local token usage for the selected range. It is an estimate, not a measured charge.'
              : `The headline, graph, and price fields are for ${compactModel(selectedModel)} in the selected range.`}
            {selectedModel !== ALL_MODELS && selectedCost.source ? ` Default source: ${selectedCost.source}.` : ''}
          </p>
          </div>
        </details>
      </Panel>

      <Panel>
        <SectionTitle title="Per-model usage" aside="lifetime" />
        {usage.length === 0 ? (
          <p className="mt-3 text-sm text-text-muted">No persisted usage yet.</p>
        ) : (
          <div className="mt-3 overflow-x-auto">
            <table className="w-full min-w-[720px] text-left text-sm">
              <thead>
                <tr className="border-b border-white/10 text-xs uppercase tracking-wide text-text-muted">
                  <th className="py-2 pr-4 font-medium">Model</th>
                  <th className="py-2 pr-4 font-medium">Requests</th>
                  <th className="py-2 pr-4 font-medium">Prompt</th>
                  <th className="py-2 pr-4 font-medium">Output</th>
                  <th className="py-2 pr-4 font-medium">Avg t/s</th>
                  <th className="py-2 pr-4 font-medium">Peak t/s</th>
                  <th className="py-2 font-medium">Est. API cost</th>
                </tr>
              </thead>
              <tbody className="divide-y divide-white/5">
                {usage.map(row => {
                  const cost = getCostConfigForModel(row.model, saved, defaults.defaults, defaults.fallback);
                  const avoided = (row.promptTokens / 1_000_000) * cost.promptPerMillion
                    + (row.completionTokens / 1_000_000) * cost.outputPerMillion;
                  return (
                    <tr key={row.model}>
                      <td className="py-2 pr-4 font-mono text-text-primary">{compactModel(row.model)}</td>
                      <td className="py-2 pr-4 text-text-secondary">{row.requests} <span className="text-text-muted">({row.successfulRequests} ok)</span></td>
                      <td className="py-2 pr-4 text-text-secondary">{formatTokenCount(row.promptTokens)}</td>
                      <td className="py-2 pr-4 text-text-secondary">{formatTokenCount(row.completionTokens)}</td>
                      <td className="py-2 pr-4 text-text-secondary">{row.avgTokensPerSecond ? row.avgTokensPerSecond.toFixed(1) : '—'}</td>
                      <td className="py-2 pr-4 text-text-secondary">{row.peakTokensPerSecond ? row.peakTokensPerSecond.toFixed(1) : '—'}</td>
                      <td className="py-2 text-success-green">{formatCurrency(avoided)}</td>
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
