import React, { useEffect, useMemo, useState } from 'react';
import { getJobs, getPricing } from '../api';
import { UsageLineChart, UsageRangeTabs } from '../components/UsageCharts';
import { Badge, EmptyState, Panel, SectionTitle, Stat } from '../components/ui';
import {
  ALL_MODELS,
  DEFAULT_COST_CONFIG,
  MODEL_COST_DEFAULTS_VERSION,
  TOKEN_RANGE_LABELS,
  buildCostDefaults,
  buildTokenSeries,
  estimateUsageCost,
  getCostConfigForModel,
  loadCostConfig,
  saveCostConfig,
  type CostDefaults,
  type ModelCostConfig,
  type TokenRange,
} from '../cost';
import {
  bucketUsageForSection,
  modelNameLooksLikeDictation,
  modelsForSection,
  usageForSection,
} from '../dashboardSections';
import { useGateway } from '../gateway';
import type { JobRecord } from '../types';
import { compactModel, formatCurrency, formatDate, formatDuration, timeAgo } from '../utils';

export const DictationUsagePage: React.FC = () => {
  const { status, models } = useGateway();
  const [jobs, setJobs] = useState<JobRecord[]>([]);
  const [defaults, setDefaults] = useState<{ defaults: CostDefaults; fallback: ModelCostConfig }>({ defaults: {}, fallback: DEFAULT_COST_CONFIG });
  const [saved, setSaved] = useState<Record<string, ModelCostConfig>>({});
  const [selectedModel, setSelectedModel] = useState('');
  const [range, setRange] = useState<TokenRange>('all');

  useEffect(() => {
    let active = true;
    getJobs(500).then(rows => { if (active) setJobs(rows); }).catch(() => {});
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

  const speechModels = useMemo(() => modelsForSection(models, 'dictation'), [models]);
  const speechIds = useMemo(() => new Set(speechModels.map(model => model.id)), [speechModels]);
  const usage = useMemo(
    () => usageForSection(status?.tokenUsage ?? [], models, 'dictation'),
    [status?.tokenUsage, models],
  );
  const monthly = useMemo(
    () => bucketUsageForSection(status?.monthlyTokenUsage ?? [], models, 'dictation'),
    [status?.monthlyTokenUsage, models],
  );
  const daily = useMemo(
    () => bucketUsageForSection(status?.dailyTokenUsage ?? [], models, 'dictation'),
    [status?.dailyTokenUsage, models],
  );
  const hourly = useMemo(
    () => bucketUsageForSection(status?.hourlyTokenUsage ?? [], models, 'dictation'),
    [status?.hourlyTokenUsage, models],
  );
  const speechJobs = useMemo(
    () => jobs.filter(job => speechIds.has(job.model) || modelNameLooksLikeDictation(job.model)),
    [jobs, speechIds],
  );
  const modelNames = useMemo(() => {
    const names = new Set<string>();
    for (const model of speechModels) names.add(model.id);
    for (const row of usage) names.add(row.model);
    return Array.from(names);
  }, [speechModels, usage]);

  useEffect(() => {
    if (!selectedModel || !modelNames.includes(selectedModel)) setSelectedModel(modelNames[0] || '');
  }, [modelNames, selectedModel]);

  const rangeSeries = useMemo(
    () => buildTokenSeries(
      speechJobs,
      ALL_MODELS,
      DEFAULT_COST_CONFIG,
      monthly,
      saved,
      defaults.defaults,
      defaults.fallback,
      range,
      daily,
      hourly,
      Boolean(status?.dailyTokenUsageAllTime),
    ),
    [speechJobs, monthly, saved, defaults, range, daily, hourly, status?.dailyTokenUsageAllTime],
  );
  const totalRequests = rangeSeries.requests.reduce((sum, value) => sum + value, 0);
  const successful = rangeSeries.successfulRequests.reduce((sum, value) => sum + value, 0);
  const failed = Math.max(0, totalRequests - successful);
  const audioSeconds = rangeSeries.audioSeconds.reduce((sum, value) => sum + value, 0);
  const characters = rangeSeries.characters.reduce((sum, value) => sum + value, 0);
  const estimatedCost = rangeSeries.cost.reduce((sum, value) => sum + value, 0);
  const effectiveSelectedModel = selectedModel || modelNames[0] || '';
  const selectedCost = effectiveSelectedModel
    ? getCostConfigForModel(effectiveSelectedModel, saved, defaults.defaults, defaults.fallback)
    : DEFAULT_COST_CONFIG;

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
        <SectionTitle title="Dictation usage" aside={TOKEN_RANGE_LABELS[range]} />
        <div className="mt-3 grid grid-cols-2 gap-4 sm:grid-cols-3 xl:grid-cols-6">
          <Stat label="Requests" value={totalRequests.toLocaleString()} />
          <Stat label="Successful" value={successful.toLocaleString()} tone={successful ? 'good' : 'idle'} />
          <Stat label="Failed" value={failed.toLocaleString()} tone={failed ? 'critical' : 'idle'} />
          <Stat label="Audio transcribed" value={formatAudio(audioSeconds)} />
          <Stat label="Text synthesized" value={formatCharacters(characters)} />
          <Stat label="Estimated API cost" value={formatCurrency(estimatedCost)} tone="good" />
        </div>
        <div className="mt-4"><UsageRangeTabs value={range} onChange={setRange} /></div>
        <p className="mt-3 text-xs text-text-muted">
          Requests, input audio duration, and synthesized characters come from the persisted SQL ledger. Cost is the comparable hosted API value, not a measured charge.
        </p>
      </Panel>

      <Panel>
        <SectionTitle title="Request volume" aside={TOKEN_RANGE_LABELS[range]} />
        <UsageLineChart
          labels={rangeSeries.months}
          series={[{ label: 'Dictation', color: '#A78BFA', values: rangeSeries.requests }]}
          ariaLabel={`Dictation requests for ${TOKEN_RANGE_LABELS[range]}`}
        />

        <details className="mt-4 border-t border-border-slate pt-3">
          <summary className="cursor-pointer text-sm font-medium text-text-secondary">Cost assumptions</summary>
          <div className="mt-3 grid gap-3 sm:grid-cols-3">
            <label className="text-xs text-text-muted">
              Model
              <select className="mt-1 h-9 w-full bg-[#07101d] px-2 text-sm text-text-primary" value={effectiveSelectedModel} onChange={event => setSelectedModel(event.target.value)}>
                {modelNames.map(name => <option key={name} value={name}>{name}</option>)}
              </select>
            </label>
            <label className="text-xs text-text-muted">
              Comparable hosted model
              <input className="mt-1 h-9 w-full bg-[#07101d] px-2 text-sm text-text-primary" value={selectedCost.equivalentModel} onChange={event => persistConfig(effectiveSelectedModel, { ...selectedCost, equivalentModel: event.target.value })} />
            </label>
            <label className="text-xs text-text-muted">
              {selectedCost.billingUnit === 'million_characters' ? 'USD / 1M characters' : 'USD / audio minute'}
              <input className="mt-1 h-9 w-full bg-[#07101d] px-2 text-sm text-text-primary" type="number" min="0" step="0.001" value={selectedCost.pricePerUnit ?? 0} onChange={event => persistConfig(effectiveSelectedModel, { ...selectedCost, pricePerUnit: Number(event.target.value) || 0 })} />
            </label>
          </div>
          <p className="mt-2 text-xs text-text-muted">
            {selectedCost.sourceUrl
              ? <>Default comparison: <a className="text-queue-blue hover:underline" href={selectedCost.sourceUrl} target="_blank" rel="noreferrer">{selectedCost.equivalentModel} official pricing</a>.</>
              : `Comparison: ${selectedCost.equivalentModel}.`}
          </p>
        </details>
      </Panel>

      <Panel>
        <SectionTitle title="Per-model usage" aside="lifetime" />
        {usage.length === 0 ? (
          <div className="mt-3"><EmptyState title="No persisted dictation usage" /></div>
        ) : (
          <div className="mt-3 overflow-x-auto" role="region" aria-label="Per-model dictation usage" tabIndex={0}>
            <table className="w-full min-w-[860px] text-left text-sm">
              <thead>
                <tr className="border-b border-white/10 text-xs uppercase tracking-wide text-text-muted">
                  <th className="py-2 pr-4 font-medium">Model</th>
                  <th className="py-2 pr-4 font-medium">Requests</th>
                  <th className="py-2 pr-4 font-medium">Success</th>
                  <th className="py-2 pr-4 font-medium">Billable work</th>
                  <th className="py-2 pr-4 font-medium">Equivalent</th>
                  <th className="py-2 pr-4 font-medium">Est. API cost</th>
                  <th className="py-2 font-medium">Last used</th>
                </tr>
              </thead>
              <tbody className="divide-y divide-white/5">
                {usage.map(row => {
                  const cost = getCostConfigForModel(row.model, saved, defaults.defaults, defaults.fallback);
                  const avoided = estimateUsageCost(row, cost);
                  const billable = cost.billingUnit === 'million_characters'
                    ? formatCharacters(Number(row.inputCharacters ?? 0))
                    : formatAudio(Number(row.inputAudioSeconds ?? 0));
                  return (
                    <tr key={row.model}>
                      <td className="py-2.5 pr-4 font-mono text-text-primary">{compactModel(row.model)}</td>
                      <td className="py-2.5 pr-4 text-text-secondary">{row.requests.toLocaleString()}</td>
                      <td className="py-2.5 pr-4 text-text-secondary">{row.requests ? `${(row.successfulRequests / row.requests * 100).toFixed(1)}%` : '—'}</td>
                      <td className="py-2.5 pr-4 text-text-secondary">{billable}</td>
                      <td className="py-2.5 pr-4 text-text-secondary">{cost.equivalentModel}</td>
                      <td className="py-2.5 pr-4 text-success-green">{formatCurrency(avoided)}</td>
                      <td className="py-2.5 text-text-secondary">{row.lastTimestampUnixMs ? timeAgo(row.lastTimestampUnixMs) : 'Never'}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </Panel>

      <Panel>
        <SectionTitle title="Recent dictation requests" aside={`latest ${speechJobs.length}`} />
        {speechJobs.length === 0 ? (
          <div className="mt-3"><EmptyState title="No recent dictation requests" /></div>
        ) : (
          <div className="mt-3 divide-y divide-white/5">
            {speechJobs.slice(0, 25).map(job => (
              <div key={job.id} className="grid gap-2 py-2.5 text-sm sm:grid-cols-[1fr_auto_auto_auto] sm:items-center">
                <span className="min-w-0 truncate font-mono text-text-primary">{compactModel(job.model)}</span>
                <span className="text-text-muted">{formatDate(job.timestampUnixMs)}</span>
                <span className="text-text-secondary">{formatDuration(job.durationMs)}</span>
                <Badge label={job.status === 'succeeded' ? 'Completed' : `Failed ${job.httpStatus}`} tone={job.status === 'succeeded' ? 'good' : 'critical'} />
              </div>
            ))}
          </div>
        )}
      </Panel>
    </div>
  );
};

function formatAudio(seconds: number): string {
  if (!seconds) return '0 min';
  return `${(seconds / 60).toLocaleString(undefined, { maximumFractionDigits: 1 })} min`;
}

function formatCharacters(characters: number): string {
  if (characters >= 1_000_000) return `${(characters / 1_000_000).toFixed(2)}M chars`;
  if (characters >= 1_000) return `${(characters / 1_000).toFixed(1)}K chars`;
  return `${characters.toLocaleString()} chars`;
}
