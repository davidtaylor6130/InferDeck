import type { JobRecord, MonthlyUsageRow, PricingEntry, UsageRow } from './types';

export interface ModelCostConfig {
  equivalentModel: string;
  promptPerMillion: number;
  outputPerMillion: number;
  breakEvenTarget: number;
  source?: string;
  defaultsVersion?: number;
  userEdited?: boolean;
  billingUnit?: 'tokens' | 'audio_minute' | 'million_characters';
  pricePerUnit?: number;
  sourceUrl?: string;
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
  cachedPrompt: number[];
  cost: number[];
  requests: number[];
  successfulRequests: number[];
  audioSeconds: number[];
  characters: number[];
  generationDurationMs: number[];
  promptDurationMs: number[];
  measuredCompletionTokens: number[];
  measuredPromptTokens: number[];
  peakTokensPerSecond: number[];
  peakPromptTokensPerSecond: number[];
}

export type TokenRange = 'day' | 'week' | 'month' | 'year' | 'all';

export const ALL_MODELS = 'All tracked models';
export const COST_STORAGE_KEY = 'inferdeck:model-token-costs';
export const DEFAULT_BREAK_EVEN_TARGET = 1739;
export const MODEL_COST_DEFAULTS_VERSION = 5;

export const TOKEN_RANGE_LABELS: Record<TokenRange, string> = {
  day: '24h',
  week: 'Week',
  month: 'Month',
  year: 'Year',
  all: 'All time',
};

export const DEFAULT_COST_CONFIG: ModelCostConfig = {
  equivalentModel: 'Not configured',
  promptPerMillion: 0,
  outputPerMillion: 0,
  breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET,
  source: 'No server-side model pricing',
  defaultsVersion: MODEL_COST_DEFAULTS_VERSION,
  billingUnit: 'tokens',
  pricePerUnit: 0,
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
      source: entry.source === 'model_settings' ? 'Model Settings' : 'data/pricing.json',
      defaultsVersion: MODEL_COST_DEFAULTS_VERSION,
      billingUnit: entry.billing_unit || 'tokens',
      pricePerUnit: sanitizeMoney(entry.price_per_unit, 0),
      sourceUrl: entry.source_url,
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
  const breakEvenTarget = config?.breakEvenTarget === undefined || !Number.isFinite(savedBreakEven)
    ? defaultConfig.breakEvenTarget
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
    billingUnit: source?.billingUnit || defaultConfig.billingUnit || 'tokens',
    pricePerUnit: sanitizeMoney(source?.pricePerUnit, defaultConfig.pricePerUnit || 0),
    sourceUrl: typeof source?.sourceUrl === 'string' && source.sourceUrl.trim()
      ? source.sourceUrl
      : defaultConfig.sourceUrl,
  };
}

export function getCostConfigForModel(
  model: string,
  saved: Record<string, ModelCostConfig>,
  defaults: CostDefaults,
  fallback: ModelCostConfig = DEFAULT_COST_CONFIG,
): ModelCostConfig {
  const defaultConfig = defaults[model] || inferredSpeechCostDefault(model) || fallback;
  return normalizeCostConfig(model, saved[model], defaultConfig);
}

export function inferredSpeechCostDefault(model: string): ModelCostConfig | undefined {
  if (/(?:whisper|parakeet|transcri|(?:^|[-_.])(stt|asr)(?:$|[-_.]))/i.test(model)) {
    return {
      equivalentModel: 'OpenAI whisper-1',
      promptPerMillion: 0,
      outputPerMillion: 0,
      breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET,
      source: 'OpenAI model pricing',
      sourceUrl: 'https://developers.openai.com/api/docs/models/whisper-1',
      defaultsVersion: MODEL_COST_DEFAULTS_VERSION,
      billingUnit: 'audio_minute',
      pricePerUnit: 0.006,
    };
  }
  if (/(?:supertonic|sapi|(?:^|[-_.])tts(?:$|[-_.])|text[-_. ]to[-_. ]speech)/i.test(model)) {
    return {
      equivalentModel: 'OpenAI tts-1',
      promptPerMillion: 0,
      outputPerMillion: 0,
      breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET,
      source: 'OpenAI model pricing',
      sourceUrl: 'https://developers.openai.com/api/docs/models/tts-1',
      defaultsVersion: MODEL_COST_DEFAULTS_VERSION,
      billingUnit: 'million_characters',
      pricePerUnit: 15,
    };
  }
  return undefined;
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

export function estimateUsageCost(
  usage: Pick<UsageRow | MonthlyUsageRow, 'promptTokens' | 'completionTokens' | 'inputAudioSeconds' | 'inputCharacters'>,
  cost: ModelCostConfig,
): number {
  if (cost.billingUnit === 'audio_minute') {
    return (Number(usage.inputAudioSeconds ?? 0) / 60) * Number(cost.pricePerUnit ?? 0);
  }
  if (cost.billingUnit === 'million_characters') {
    return (Number(usage.inputCharacters ?? 0) / 1_000_000) * Number(cost.pricePerUnit ?? 0);
  }
  const prompt = Number(usage.promptTokens ?? 0);
  const output = Number(usage.completionTokens ?? 0);
  return estimateCostAvoided(
    { model: '', prompt, output, total: prompt + output },
    cost,
  );
}

export function estimatePortfolioCostAvoided(
  usage: UsageRow[],
  saved: Record<string, ModelCostConfig>,
  defaults: CostDefaults,
  fallback: ModelCostConfig,
): number {
  return usage.reduce((sum, row) => {
    return sum + estimateUsageCost(
      row,
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
  daily: MonthlyUsageRow[] = [],
  hourly: MonthlyUsageRow[] = [],
  dailyAllTime = false,
): TokenSeries {
  const persistedRows = selectPersistedRows(range, model, persisted, daily, hourly, dailyAllTime);
  const buckets = buildTokenBuckets(range, jobs, model, persistedRows);
  const byBucket = new Map(buckets.map(bucket => [bucket.key, {
    prompt: 0,
    cachedPrompt: 0,
    output: 0,
    total: 0,
    cost: 0,
    requests: 0,
    successfulRequests: 0,
    audioSeconds: 0,
    characters: 0,
    generationDurationMs: 0,
    promptDurationMs: 0,
    measuredCompletionTokens: 0,
    measuredPromptTokens: 0,
    peakTokensPerSecond: 0,
    peakPromptTokensPerSecond: 0,
  }]));
  const relevantPersistedRows = persistedRows.filter(row => model === ALL_MODELS || row.model === model);
  const usePersisted = relevantPersistedRows.length > 0;
  if (usePersisted) {
    for (const row of relevantPersistedRows) {
      let bucket = byBucket.get(row.bucket);
      if (!bucket) {
        const chartBucket = findTokenBucket(buckets, bucketStartDate(row.bucket));
        bucket = chartBucket ? byBucket.get(chartBucket.key) : undefined;
      }
      if (!bucket) continue;
      const prompt = Number(row.promptTokens ?? 0);
      const output = Number(row.completionTokens ?? 0);
      bucket.prompt += prompt;
      bucket.cachedPrompt += Number(row.cachedPromptTokens ?? 0);
      bucket.output += output;
      bucket.total += Number(row.totalTokens ?? prompt + output);
      bucket.requests += Number(row.requests ?? 0);
      bucket.successfulRequests += Number(row.successfulRequests ?? 0);
      bucket.audioSeconds += Number(row.inputAudioSeconds ?? 0);
      bucket.characters += Number(row.inputCharacters ?? 0);
      bucket.generationDurationMs += Number(row.generationDurationMs ?? 0);
      bucket.promptDurationMs += Number(row.promptDurationMs ?? 0);
      bucket.measuredCompletionTokens += Number(row.measuredCompletionTokens ?? 0);
      bucket.measuredPromptTokens += Number(row.measuredPromptTokens ?? 0);
      bucket.peakTokensPerSecond = Math.max(bucket.peakTokensPerSecond, Number(row.peakTokensPerSecond ?? 0));
      bucket.peakPromptTokensPerSecond = Math.max(bucket.peakPromptTokensPerSecond, Number(row.peakPromptTokensPerSecond ?? 0));
      bucket.cost += estimateUsageCost(
        row,
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
      const uncachedPrompt = Math.max(0, prompt - Number(job.cachedPromptTokens ?? 0));
      const output = Number(job.completionTokens ?? 0);
      const jobModel = job.model || 'Unknown model';
      bucket.prompt += prompt;
      bucket.output += output;
      bucket.total += Number(job.totalTokens ?? prompt + output);
      bucket.requests += 1;
      bucket.successfulRequests += job.status === 'succeeded' ? 1 : 0;
      bucket.audioSeconds += Number(job.inputAudioSeconds ?? 0);
      bucket.characters += Number(job.inputCharacters ?? 0);
      const generationDurationMs = Number(job.generationDurationMs ?? 0);
      const promptDurationMs = Number(job.promptDurationMs ?? 0);
      if (job.status === 'succeeded' && generationDurationMs > 0 && output > 0) {
        bucket.generationDurationMs += generationDurationMs;
        bucket.measuredCompletionTokens += output;
        bucket.peakTokensPerSecond = Math.max(bucket.peakTokensPerSecond, output / (generationDurationMs / 1000));
      }
      if (job.status === 'succeeded' && promptDurationMs > 0 && uncachedPrompt > 0) {
        bucket.promptDurationMs += promptDurationMs;
        bucket.measuredPromptTokens += uncachedPrompt;
        bucket.peakPromptTokensPerSecond = Math.max(bucket.peakPromptTokensPerSecond, uncachedPrompt / (promptDurationMs / 1000));
      }
      bucket.cost += estimateUsageCost(
        {
          promptTokens: prompt,
          completionTokens: output,
          inputAudioSeconds: job.inputAudioSeconds,
          inputCharacters: job.inputCharacters,
        },
        model === ALL_MODELS ? getCostConfigForModel(jobModel, saved, defaults, fallback) : cost,
      );
    }
  }
  const values = buckets.map(bucket => byBucket.get(bucket.key) || {
    prompt: 0,
    cachedPrompt: 0,
    output: 0,
    total: 0,
    cost: 0,
    requests: 0,
    successfulRequests: 0,
    audioSeconds: 0,
    characters: 0,
    generationDurationMs: 0,
    promptDurationMs: 0,
    measuredCompletionTokens: 0,
    measuredPromptTokens: 0,
    peakTokensPerSecond: 0,
    peakPromptTokensPerSecond: 0,
  });
  return {
    months: buckets.map(bucket => bucket.label),
    total: values.map(value => value.total),
    prompt: values.map(value => value.prompt),
    cachedPrompt: values.map(value => value.cachedPrompt),
    output: values.map(value => value.output),
    cost: values.map(value => value.cost),
    requests: values.map(value => value.requests),
    successfulRequests: values.map(value => value.successfulRequests),
    audioSeconds: values.map(value => value.audioSeconds),
    characters: values.map(value => value.characters),
    generationDurationMs: values.map(value => value.generationDurationMs),
    promptDurationMs: values.map(value => value.promptDurationMs),
    measuredCompletionTokens: values.map(value => value.measuredCompletionTokens),
    measuredPromptTokens: values.map(value => value.measuredPromptTokens),
    peakTokensPerSecond: values.map(value => value.peakTokensPerSecond),
    peakPromptTokensPerSecond: values.map(value => value.peakPromptTokensPerSecond),
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
  if (range === 'day') return fixedHourBuckets(24);
  if (range === 'week') return fixedDayBuckets(7);
  if (range === 'month') return fixedDayBuckets(30, 5);
  if (range === 'year') return fixedMonthBuckets(12);
  return allTimeTokenBuckets(jobs, model, persisted);
}

function selectPersistedRows(
  range: TokenRange,
  model: string,
  monthly: MonthlyUsageRow[],
  daily: MonthlyUsageRow[],
  hourly: MonthlyUsageRow[],
  dailyAllTime: boolean,
): MonthlyUsageRow[] {
  if (range === 'day') return hourly;
  if (range === 'week' || range === 'month') return daily;
  if (range === 'year') return monthly;
  const relevant = (rows: MonthlyUsageRow[]) =>
    rows.filter(row => model === ALL_MODELS || row.model === model);
  const bucketCount = (rows: MonthlyUsageRow[]) =>
    new Set(relevant(rows).map(row => row.bucket)).size;
  if (new Set(monthly.map(row => row.bucket)).size >= 12) return monthly;
  if (dailyAllTime && bucketCount(daily) >= 12) return daily;
  if (bucketCount(hourly) >= 12) return hourly;
  if (relevant(monthly).length) return monthly;
  if (relevant(daily).length) return daily;
  return hourly;
}

function fixedHourBuckets(hours: number): TokenBucket[] {
  const now = new Date();
  const currentHour = new Date(now.getFullYear(), now.getMonth(), now.getDate(), now.getHours());
  return Array.from({ length: hours }, (_, index) => {
    const start = new Date(currentHour.getTime() - (hours - 1 - index) * 3_600_000);
    const end = new Date(start.getTime() + 3_600_000);
    return {
      key: `${dateKey(start)}T${String(start.getHours()).padStart(2, '0')}`,
      label: `${String(start.getHours()).padStart(2, '0')}:00`,
      start,
      end,
    };
  });
}

function bucketStartDate(bucket: string): Date {
  const [datePart, hourPart] = bucket.split('T');
  const [year, month, day] = datePart.split('-').map(Number);
  return new Date(year, (month || 1) - 1, day || 1, Number(hourPart || 0));
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

function fixedMonthBuckets(months: number): TokenBucket[] {
  const now = new Date();
  const current = new Date(now.getFullYear(), now.getMonth(), 1);
  return Array.from({ length: months }, (_, index) => {
    const start = addMonths(current, -(months - 1 - index));
    return {
      key: monthKey(start),
      label: monthLabel(monthKey(start)),
      start,
      end: addMonths(start, 1),
    };
  });
}

function allTimeTokenBuckets(
  jobs: JobRecord[],
  model: string,
  persisted: MonthlyUsageRow[],
): TokenBucket[] {
  const points = new Map<number, Date>();
  for (const row of persisted) {
    if (model !== ALL_MODELS && row.model !== model) continue;
    const date = bucketStartDate(row.bucket);
    if (!Number.isNaN(date.getTime())) points.set(date.getTime(), date);
  }
  if (!points.size) {
    for (const job of jobs) {
      if (model !== ALL_MODELS && (job.model || 'Unknown model') !== model) continue;
      const date = new Date(job.timestampUnixMs || job.createdAt);
      if (!Number.isNaN(date.getTime())) points.set(date.getTime(), date);
    }
  }
  if (!points.size) {
    const now = startOfDay(new Date());
    return [{ key: 'all:0', label: compactDateLabel(now), start: now, end: addDays(now, 1) }];
  }
  const dates = Array.from(points.values()).sort((left, right) => left.getTime() - right.getTime());
  const chunk = Math.max(1, Math.ceil(dates.length / 24));
  const buckets: TokenBucket[] = [];
  for (let offset = 0; offset < dates.length; offset += chunk) {
    const group = dates.slice(offset, offset + chunk);
    const start = group[0];
    const last = group[group.length - 1];
    const next = dates[offset + chunk];
    const fallbackEnd = persisted[0]?.bucket.includes('T')
      ? new Date(last.getTime() + 3_600_000)
      : /^\d{4}-\d{2}$/.test(persisted[0]?.bucket ?? '')
        ? addMonths(last, 1)
        : addDays(last, 1);
    const firstLabel = compactDateLabel(start, persisted[0]?.bucket);
    const lastLabel = compactDateLabel(last, persisted[0]?.bucket);
    buckets.push({
      key: `all:${buckets.length}`,
      label: firstLabel === lastLabel ? firstLabel : `${firstLabel}–${lastLabel}`,
      start,
      end: next ?? fallbackEnd,
    });
  }
  return buckets;
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

function compactDateLabel(date: Date, sourceBucket = ''): string {
  if (sourceBucket.includes('T')) {
    return date.toLocaleString([], { month: 'short', day: 'numeric', hour: '2-digit' });
  }
  if (/^\d{4}-\d{2}$/.test(sourceBucket)) return monthLabel(monthKey(date));
  return date.toLocaleString([], { month: 'short', day: 'numeric' });
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
