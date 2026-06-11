import type { JobRecord, MonthlyUsageRow, PricingEntry, UsageRow } from './types';

export interface ModelCostConfig {
  equivalentModel: string;
  promptPerMillion: number;
  outputPerMillion: number;
  breakEvenTarget: number;
  source?: string;
  defaultsVersion?: number;
  userEdited?: boolean;
}

export interface ModelTokenUsage {
  model: string;
  prompt: number;
  output: number;
  total: number;
}

export interface TokenSeries {
  months: string[];
  total: number[];
  prompt: number[];
  output: number[];
  cost: number[];
}

export type TokenRange = 'week' | 'month' | 'year' | 'all';

export const ALL_MODELS = 'All tracked models';
export const COST_STORAGE_KEY = 'inferdeck:model-token-costs';
export const DEFAULT_BREAK_EVEN_TARGET = 1739;
export const MODEL_COST_DEFAULTS_VERSION = 4;

export const TOKEN_RANGE_LABELS: Record<TokenRange, string> = {
  week: 'Week',
  month: 'Month',
  year: 'Year',
  all: 'All time',
};

export const DEFAULT_COST_CONFIG: ModelCostConfig = {
  equivalentModel: 'OpenRouter median tracked chat model',
  promptPerMillion: 0.55,
  outputPerMillion: 1.44,
  breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET,
  source: 'OpenRouter median chat model pricing',
  defaultsVersion: MODEL_COST_DEFAULTS_VERSION,
};

const LEGACY_DEFAULT_PRICES: Record<string, Array<[number, number]>> = {
  'qwen3.6-27b': [[0.455, 1.82], [0.325, 1.95]],
  'qwen3.6-35b-a3b': [[0.455, 1.82], [0.325, 1.95], [0.129, 0.512]],
  'qwen3-coder-30b-a3b': [[0.455, 1.82]],
  'qwen2.5-coder-3b': [[0.30, 0.30]],
  'gemma-4-26b-a4b': [[0.10, 0.30]],
  'gpt-oss-20b': [[0.05, 0.20]],
};

export type CostDefaults = Record<string, ModelCostConfig>;

export function buildCostDefaults(pricing: PricingEntry[]): { defaults: CostDefaults; fallback: ModelCostConfig } {
  const defaults: CostDefaults = {};
  let fallback = DEFAULT_COST_CONFIG;
  for (const entry of pricing) {
    if (!entry?.model_name) continue;
    const config: ModelCostConfig = {
      equivalentModel: entry.equivalent_api_model || DEFAULT_COST_CONFIG.equivalentModel,
      promptPerMillion: sanitizeMoney(entry.prompt_price_per_million, DEFAULT_COST_CONFIG.promptPerMillion),
      outputPerMillion: sanitizeMoney(entry.completion_price_per_million, DEFAULT_COST_CONFIG.outputPerMillion),
      breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET,
      source: 'data/pricing.json',
      defaultsVersion: MODEL_COST_DEFAULTS_VERSION,
    };
    if (entry.model_name === 'default') {
      fallback = config;
    } else {
      defaults[entry.model_name] = config;
    }
  }
  return { defaults, fallback };
}

export function loadCostConfig(defaults: CostDefaults, fallback: ModelCostConfig): Record<string, ModelCostConfig> {
  if (typeof window === 'undefined') return {};
  try {
    const parsed = JSON.parse(window.localStorage.getItem(COST_STORAGE_KEY) || '{}') as Record<string, ModelCostConfig>;
    if (!parsed || typeof parsed !== 'object') return {};
    return Object.fromEntries(Object.entries(parsed).map(([model, config]) =>
      [model, normalizeCostConfig(model, config, defaults[model] || fallback)]));
  } catch {
    return {};
  }
}

export function saveCostConfig(config: Record<string, ModelCostConfig>) {
  if (typeof window === 'undefined') return;
  window.localStorage.setItem(COST_STORAGE_KEY, JSON.stringify(config));
}

export function normalizeCostConfig(
  model: string,
  config: Partial<ModelCostConfig> | undefined,
  defaultConfig: ModelCostConfig,
): ModelCostConfig {
  const shouldRefreshDefaults =
    defaultConfig !== DEFAULT_COST_CONFIG &&
    config &&
    !config.userEdited &&
    Number(config.defaultsVersion ?? 0) < MODEL_COST_DEFAULTS_VERSION &&
    isLegacyDefaultConfig(model, config);
  const source = shouldRefreshDefaults ? defaultConfig : config;
  const savedBreakEven = Number(config?.breakEvenTarget);
  const breakEvenTarget = !Number.isFinite(savedBreakEven) || savedBreakEven === 0
    ? DEFAULT_BREAK_EVEN_TARGET
    : sanitizeMoney(config?.breakEvenTarget, defaultConfig.breakEvenTarget);
  return {
    equivalentModel: typeof source?.equivalentModel === 'string' && source.equivalentModel.trim()
      ? source.equivalentModel
      : defaultConfig.equivalentModel,
    promptPerMillion: sanitizeMoney(source?.promptPerMillion, defaultConfig.promptPerMillion),
    outputPerMillion: sanitizeMoney(source?.outputPerMillion, defaultConfig.outputPerMillion),
    breakEvenTarget,
    source: typeof source?.source === 'string' && source.source.trim() ? source.source : defaultConfig.source,
    defaultsVersion: Number(source?.defaultsVersion ?? defaultConfig.defaultsVersion ?? MODEL_COST_DEFAULTS_VERSION),
    userEdited: Boolean(config?.userEdited),
  };
}

export function getCostConfigForModel(
  model: string,
  saved: Record<string, ModelCostConfig>,
  defaults: CostDefaults,
  fallback: ModelCostConfig = DEFAULT_COST_CONFIG,
): ModelCostConfig {
  const defaultConfig = defaults[model] || fallback;
  return normalizeCostConfig(model, saved[model], defaultConfig);
}

function sanitizeMoney(value: unknown, fallback: number): number {
  const number = Number(value);
  return Number.isFinite(number) && number >= 0 ? number : fallback;
}

function isLegacyDefaultConfig(model: string, config: Partial<ModelCostConfig>): boolean {
  if (!config.defaultsVersion && config.equivalentModel === DEFAULT_COST_CONFIG.equivalentModel) return true;
  const prompt = Number(config.promptPerMillion);
  const output = Number(config.outputPerMillion);
  return (LEGACY_DEFAULT_PRICES[model] || []).some(([legacyPrompt, legacyOutput]) =>
    Math.abs(prompt - legacyPrompt) < 0.0001 && Math.abs(output - legacyOutput) < 0.0001,
  );
}

export function estimateCostAvoided(usage: ModelTokenUsage, cost: ModelCostConfig): number {
  return (usage.prompt / 1_000_000) * cost.promptPerMillion + (usage.output / 1_000_000) * cost.outputPerMillion;
}

export function estimatePortfolioCostAvoided(
  usage: UsageRow[],
  saved: Record<string, ModelCostConfig>,
  defaults: CostDefaults,
  fallback: ModelCostConfig,
): number {
  return usage.reduce((sum, row) => {
    const prompt = Number(row.promptTokens ?? 0);
    const output = Number(row.completionTokens ?? 0);
    return sum + estimateCostAvoided(
      { model: row.model, prompt, output, total: prompt + output },
      getCostConfigForModel(row.model, saved, defaults, fallback),
    );
  }, 0);
}

type TokenBucket = { key: string; label: string; start: Date; end: Date };

export function buildTokenSeries(
  jobs: JobRecord[],
  model: string,
  cost: ModelCostConfig,
  persisted: MonthlyUsageRow[],
  saved: Record<string, ModelCostConfig>,
  defaults: CostDefaults,
  fallback: ModelCostConfig,
  range: TokenRange,
): TokenSeries {
  const buckets = buildTokenBuckets(range, jobs, model, persisted);
  const byBucket = new Map(buckets.map(bucket => [bucket.key, { prompt: 0, output: 0, total: 0, cost: 0 }]));
  const usePersisted = persisted.length > 0 && (range === 'all' || range === 'year');
  if (usePersisted) {
    for (const row of persisted) {
      if (model !== ALL_MODELS && row.model !== model) continue;
      const bucket = byBucket.get(row.bucket);
      if (!bucket) continue;
      const prompt = Number(row.promptTokens ?? 0);
      const output = Number(row.completionTokens ?? 0);
      bucket.prompt += prompt;
      bucket.output += output;
      bucket.total += Number(row.totalTokens ?? prompt + output);
      bucket.cost += estimateCostAvoided(
        { model: row.model, prompt, output, total: prompt + output },
        model === ALL_MODELS ? getCostConfigForModel(row.model, saved, defaults, fallback) : cost,
      );
    }
  } else {
    for (const job of jobs) {
      if (model !== ALL_MODELS && (job.model || 'Unknown model') !== model) continue;
      const date = new Date(job.timestampUnixMs || job.createdAt);
      const chartBucket = findTokenBucket(buckets, Number.isNaN(date.getTime()) ? new Date() : date);
      const bucket = chartBucket ? byBucket.get(chartBucket.key) : undefined;
      if (!bucket) continue;
      const prompt = Number(job.promptTokens ?? 0);
      const output = Number(job.completionTokens ?? 0);
      const jobModel = job.model || 'Unknown model';
      bucket.prompt += prompt;
      bucket.output += output;
      bucket.total += Number(job.totalTokens ?? prompt + output);
      bucket.cost += estimateCostAvoided(
        { model: jobModel, prompt, output, total: prompt + output },
        model === ALL_MODELS ? getCostConfigForModel(jobModel, saved, defaults, fallback) : cost,
      );
    }
  }
  const values = buckets.map(bucket => byBucket.get(bucket.key) || { prompt: 0, output: 0, total: 0, cost: 0 });
  return {
    months: buckets.map(bucket => bucket.label),
    total: values.map(value => value.total),
    prompt: values.map(value => value.prompt),
    output: values.map(value => value.output),
    cost: values.map(value => value.cost),
  };
}

export function tokenUsageFromSeries(model: string, series: TokenSeries): ModelTokenUsage {
  return {
    model,
    prompt: series.prompt.reduce((sum, value) => sum + value, 0),
    output: series.output.reduce((sum, value) => sum + value, 0),
    total: series.total.reduce((sum, value) => sum + value, 0),
  };
}

function buildTokenBuckets(range: TokenRange, jobs: JobRecord[], model: string, persisted: MonthlyUsageRow[]): TokenBucket[] {
  if (range === 'week') return fixedDayBuckets(7);
  if (range === 'month') return fixedDayBuckets(30, 5);
  return monthlyTokenBuckets(range, jobs, model, persisted);
}

function fixedDayBuckets(days: number, spanDays = 1): TokenBucket[] {
  const now = startOfDay(new Date());
  const bucketCount = Math.ceil(days / spanDays);
  const firstStart = addDays(now, -(days - 1));
  return Array.from({ length: bucketCount }, (_, index) => {
    const start = addDays(firstStart, index * spanDays);
    const end = addDays(start, spanDays);
    return {
      key: `${dateKey(start)}:${spanDays}`,
      label: spanDays === 1 ? start.toLocaleString([], { weekday: 'short' }) : start.toLocaleString([], { month: 'short', day: 'numeric' }),
      start,
      end: index === bucketCount - 1 ? addDays(now, 1) : end,
    };
  });
}

function monthlyTokenBuckets(range: TokenRange, jobs: JobRecord[], model: string, persisted: MonthlyUsageRow[]): TokenBucket[] {
  const monthKeys = new Set<string>();
  const now = new Date();
  const earliestYearMonth = range === 'year'
    ? monthKey(addMonths(new Date(now.getFullYear(), now.getMonth(), 1), -11))
    : '';
  for (const row of persisted) {
    if (model !== ALL_MODELS && row.model !== model) continue;
    if (!/^\d{4}-\d{2}$/.test(row.bucket)) continue;
    if (range === 'year' && row.bucket < earliestYearMonth) continue;
    monthKeys.add(row.bucket);
  }
  if (!monthKeys.size) {
    for (const job of jobs) {
      if (model !== ALL_MODELS && (job.model || 'Unknown model') !== model) continue;
      const date = new Date(job.timestampUnixMs || job.createdAt);
      const key = monthKey(Number.isNaN(date.getTime()) ? now : date);
      if (range === 'year' && key < earliestYearMonth) continue;
      monthKeys.add(key);
    }
  }
  if (!monthKeys.size) monthKeys.add(monthKey(now));
  return Array.from(monthKeys).sort().map(key => {
    const start = monthStart(key);
    return { key, label: monthLabel(key), start, end: addMonths(start, 1) };
  });
}

function findTokenBucket(buckets: TokenBucket[], date: Date): TokenBucket | undefined {
  const time = date.getTime();
  return buckets.find(bucket => time >= bucket.start.getTime() && time < bucket.end.getTime());
}

function monthLabel(key: string): string {
  const [year, month] = key.split('-').map(Number);
  const date = new Date(year, (month || 1) - 1, 1);
  return `${date.toLocaleString([], { month: 'short' })} ${year}`;
}

function monthKey(date: Date): string {
  return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}`;
}

function dateKey(date: Date): string {
  return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`;
}

function monthStart(key: string): Date {
  const [year, month] = key.split('-').map(Number);
  return new Date(year, (month || 1) - 1, 1);
}

function startOfDay(date: Date): Date {
  return new Date(date.getFullYear(), date.getMonth(), date.getDate());
}

function addDays(date: Date, days: number): Date {
  const next = new Date(date);
  next.setDate(next.getDate() + days);
  return next;
}

function addMonths(date: Date, months: number): Date {
  const next = new Date(date);
  next.setMonth(next.getMonth() + months);
  return next;
}
