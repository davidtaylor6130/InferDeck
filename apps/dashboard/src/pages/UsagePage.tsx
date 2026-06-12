import React, { useEffect, useMemo, useState } from 'react';
import { getJobs, getPricing } from '../api';
import { Panel, ProgressBar, SectionTitle, Stat, linePath } from '../components/ui';
import {
  ALL_MODELS,
  DEFAULT_COST_CONFIG,
  MODEL_COST_DEFAULTS_VERSION,
  TOKEN_RANGE_LABELS,
  buildCostDefaults,
  buildTokenSeries,
  estimatePortfolioCostAvoided,
  getCostConfigForModel,
  loadCostConfig,
  saveCostConfig,
  tokenUsageFromSeries,
} from '../cost';
import type { CostDefaults, ModelCostConfig, TokenRange, TokenSeries } from '../cost';
import { useGateway } from '../gateway';
import type { JobRecord } from '../types';
import { compactModel, formatCurrency, formatTokenCount } from '../utils';

export const UsagePage: React.FC = () => {
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

  const usage = status?.tokenUsage ?? [];
  const monthly = status?.monthlyTokenUsage ?? [];
  const daily = status?.dailyTokenUsage ?? [];
  const hourly = status?.hourlyTokenUsage ?? [];

  const modelNames = useMemo(() => {
    const names = new Set<string>([ALL_MODELS]);
    for (const row of usage) names.add(row.model);
    for (const model of models) names.add(model.id);
    return Array.from(names);
  }, [usage, models]);

  useEffect(() => {
    if (!modelNames.includes(selectedModel)) setSelectedModel(ALL_MODELS);
  }, [modelNames, selectedModel]);

  const selectedCost = getCostConfigForModel(selectedModel, saved, defaults.defaults, defaults.fallback);
  const portfolioCost = getCostConfigForModel(ALL_MODELS, saved, defaults.defaults, defaults.fallback);

  const series = useMemo(
    () => buildTokenSeries(jobs, selectedModel, selectedCost, monthly, saved, defaults.defaults, defaults.fallback, range, daily, hourly),
    [jobs, selectedModel, selectedCost, monthly, saved, defaults, range, daily, hourly],
  );
  const seriesUsage = useMemo(() => tokenUsageFromSeries(selectedModel, series), [selectedModel, series]);

  const portfolioCostAvoided = useMemo(
    () => estimatePortfolioCostAvoided(usage, saved, defaults.defaults, defaults.fallback),
    [usage, saved, defaults],
  );
  const roiRemaining = Math.max(0, portfolioCost.breakEvenTarget - portfolioCostAvoided);
  const roiProgress = portfolioCost.breakEvenTarget > 0
    ? Math.min(100, (portfolioCostAvoided / portfolioCost.breakEvenTarget) * 100)
    : 0;

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
        <SectionTitle title="Token usage & cost" aside={TOKEN_RANGE_LABELS[range]} />
        <div className="mt-3 grid grid-cols-2 gap-4 sm:grid-cols-5">
          <Stat label="Total tokens" value={formatTokenCount(seriesUsage.total)} />
          <Stat label="Prompt" value={formatTokenCount(seriesUsage.prompt)} />
          <Stat label="Output" value={formatTokenCount(seriesUsage.output)} />
          <Stat label="Portfolio cost avoided" value={formatCurrency(portfolioCostAvoided)} tone="good" />
          <Stat
            label="ROI remaining"
            value={portfolioCost.breakEvenTarget > 0 ? formatCurrency(roiRemaining) : 'Set target'}
            tone={roiRemaining === 0 && portfolioCost.breakEvenTarget > 0 ? 'good' : 'warn'}
          />
        </div>

        <div className="mt-3 flex flex-wrap gap-1 rounded-lg border border-white/10 bg-[#07101d] p-1 text-xs">
          {(Object.keys(TOKEN_RANGE_LABELS) as TokenRange[]).map(value => (
            <button
              key={value}
              type="button"
              className={`rounded-md px-3 py-1.5 font-medium transition ${range === value ? 'bg-queue-blue text-white' : 'text-text-muted hover:bg-white/5 hover:text-text-primary'}`}
              onClick={() => setRange(value)}
            >
              {TOKEN_RANGE_LABELS[value]}
            </button>
          ))}
        </div>

        {portfolioCost.breakEvenTarget > 0 && (
          <div className="mt-3">
            <div className="mb-1 flex justify-between text-xs text-text-muted">
              <span>Portfolio break-even progress</span>
              <span>{Math.round(roiProgress)}%</span>
            </div>
            <ProgressBar percent={roiProgress} tone="good" />
          </div>
        )}

        <TokenUsageGraph series={series} />

        <div className="mt-4 grid gap-3 rounded-lg border border-white/10 bg-[#07101d] p-3 lg:grid-cols-[1.15fr_1fr_1fr_1fr]">
          <label className="min-w-0 text-xs text-text-secondary">
            <span className="mb-1 block text-text-muted">Model</span>
            <select
              className="h-9 w-full rounded-md border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary"
              value={selectedModel}
              onChange={event => setSelectedModel(event.target.value)}
            >
              {modelNames.map(model => <option key={model} value={model}>{model}</option>)}
            </select>
          </label>
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
          <label className="min-w-0 text-xs text-text-secondary">
            <span className="mb-1 block text-text-muted">Portfolio break-even $</span>
            <input
              className="h-9 w-full rounded-md border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary"
              type="number" min="0" step="1"
              value={portfolioCost.breakEvenTarget}
              onChange={event => persistConfig(ALL_MODELS, { ...portfolioCost, breakEvenTarget: Number(event.target.value) || 0 })}
            />
          </label>
          <p className="text-xs text-text-muted lg:col-span-4">
            {selectedModel === ALL_MODELS
              ? 'Portfolio cost avoided uses each model\'s persisted tokens and saved per-model API prices; the break-even target applies to the whole tracked portfolio.'
              : `The token graph and price fields are for ${compactModel(selectedModel)}. Headline ROI always uses all tracked models and the portfolio break-even target.`}
            {selectedModel !== ALL_MODELS && selectedCost.source ? ` Default source: ${selectedCost.source}.` : ''}
          </p>
        </div>
      </Panel>

      <Panel>
        <SectionTitle title="Per-model usage" />
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
                  <th className="py-2 font-medium">Cost avoided</th>
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
  const monthX = (index: number) => series.months.length <= 1 ? 340 : (680 / (series.months.length - 1)) * index;
  return (
    <div className="mt-5">
      <div className="grid grid-cols-[34px_1fr_42px] gap-2 text-xs text-text-muted">
        <div className="flex flex-col justify-between py-2"><span>{formatTokenCount(tokenMax)}</span><span>{formatTokenCount(tokenMax / 2)}</span><span>0</span></div>
        <div>
          <svg viewBox="0 0 680 150" className="h-[150px] w-full overflow-visible" role="img" aria-label="token usage graph">
            <g stroke="rgba(148,163,184,0.14)" strokeDasharray="4 5">
              {[0, 75, 150].map(y => <line key={y} x1="0" y1={y} x2="680" y2={y} />)}
              {series.months.map((_, index) => <line key={index} x1={monthX(index)} y1="0" x2={monthX(index)} y2="150" />)}
            </g>
            <path d={linePath(series.total, 680, 150, tokenMax)} fill="none" stroke="#60A5FA" strokeWidth="3" />
            <path d={linePath(series.prompt, 680, 150, tokenMax)} fill="none" stroke="#34D399" strokeWidth="3" />
            <path d={linePath(series.output, 680, 150, tokenMax)} fill="none" stroke="#A78BFA" strokeWidth="3" />
            <path d={linePath(series.cost, 680, 150, costMax)} fill="none" stroke="#22C55E" strokeWidth="3" strokeDasharray="8 6" />
          </svg>
          <div className="mt-1 flex justify-between text-xs text-text-muted">{series.months.map(month => <span key={month}>{month}</span>)}</div>
        </div>
        <div className="flex flex-col justify-between py-2 text-right"><span>{formatCurrency(costMax)}</span><span>{formatCurrency(costMax / 2)}</span><span>$0</span></div>
      </div>
      <div className="mt-4 flex flex-wrap gap-x-5 gap-y-2 text-xs text-text-secondary">
        <Legend color="#60A5FA" label="Total Tokens" />
        <Legend color="#34D399" label="Prompt Tokens" />
        <Legend color="#A78BFA" label="Output Tokens" />
        <Legend color="#22C55E" label="Est. Cost Avoided (USD)" dashed />
      </div>
    </div>
  );
};

const Legend: React.FC<{ color: string; label: string; dashed?: boolean }> = ({ color, label, dashed }) => (
  <span className="inline-flex items-center gap-2">
    <span className="h-0.5 w-6" style={{ background: dashed ? `repeating-linear-gradient(90deg, ${color} 0 8px, transparent 8px 13px)` : color }} />
    {label}
  </span>
);
