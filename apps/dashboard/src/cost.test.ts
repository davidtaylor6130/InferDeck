import { describe, expect, it } from 'vitest';
import {
  ALL_MODELS,
  DEFAULT_COST_CONFIG,
  buildCostDefaults,
  buildTokenSeries,
  estimateCostAvoided,
  estimatePortfolioCostAvoided,
  estimateUsageCost,
  getCostConfigForModel,
  normalizeCostConfig,
  tokenUsageFromSeries,
} from './cost';
import type { JobRecord, MonthlyUsageRow, UsageRow } from './types';

const usageRow = (model: string, prompt: number, output: number): UsageRow => ({
  model,
  requests: 1,
  successfulRequests: 1,
  promptTokens: prompt,
  completionTokens: output,
  totalTokens: prompt + output,
  peakTokensPerSecond: 0,
  avgTokensPerSecond: 0,
  lastTimestampUnixMs: 0,
});

describe('buildCostDefaults', () => {
  it('maps pricing.json entries and treats "default" as the fallback', () => {
    const { defaults, fallback } = buildCostDefaults([
      { model_name: 'default', prompt_price_per_million: 0.5, cached_prompt_price_per_million: 0.05, completion_price_per_million: 1.0 },
      { model_name: 'qwen3.6-35b-a3b', prompt_price_per_million: 0.118, cached_prompt_price_per_million: 0.02, completion_price_per_million: 1.05, legacy_cached_prompt_ratio: 0.95, legacy_cached_prompt_before: '2026-08-18', equivalent_api_model: 'Equivalent API model' },
    ]);
    expect(fallback.promptPerMillion).toBe(0.5);
    expect(fallback.cachedPromptPerMillion).toBe(0.05);
    expect(defaults['qwen3.6-35b-a3b'].cachedPromptPerMillion).toBe(0.02);
    expect(defaults['qwen3.6-35b-a3b'].outputPerMillion).toBe(1.05);
    expect(defaults['qwen3.6-35b-a3b'].equivalentModel).toBe('Equivalent API model');
    expect(defaults['qwen3.6-35b-a3b'].legacyCachedPromptRatio).toBe(0.95);
    expect(defaults['qwen3.6-35b-a3b'].legacyCachedPromptBefore).toBe('2026-08-18');
  });

  it('keeps partial server pricing finite for newly configured models', () => {
    const { defaults } = buildCostDefaults([
      { model_name: 'cached-only', cached_prompt_price_per_million: 0.03 },
      { model_name: 'prompt-only', prompt_price_per_million: 0.7 },
      { model_name: 'completion-only', completion_price_per_million: 1.1 },
    ]);
    expect(defaults['cached-only']).toMatchObject({
      promptPerMillion: 0,
      cachedPromptPerMillion: 0.03,
      outputPerMillion: 0,
    });
    expect(defaults['prompt-only']).toMatchObject({
      promptPerMillion: 0.7,
      cachedPromptPerMillion: 0.7,
      outputPerMillion: 0,
    });
    expect(defaults['completion-only']).toMatchObject({
      promptPerMillion: 0,
      cachedPromptPerMillion: 0,
      outputPerMillion: 1.1,
    });
    for (const cost of Object.values(defaults)) {
      expect(Number.isFinite(estimateUsageCost(
        { promptTokens: 100, cachedPromptTokens: 50, completionTokens: 25 },
        cost,
      ))).toBe(true);
    }
  });
});

describe('estimateCostAvoided', () => {
  it('prices uncached input, cached input, and output tokens separately', () => {
    const cost = estimateCostAvoided(
      { model: 'm', prompt: 2_000_000, cachedPrompt: 500_000, output: 1_000_000, total: 3_000_000 },
      { ...DEFAULT_COST_CONFIG, promptPerMillion: 0.1, cachedPromptPerMillion: 0.02, outputPerMillion: 0.4 },
    );
    expect(cost).toBeCloseTo(0.56);
  });

  it('clamps cached input to the total prompt token count', () => {
    const cost = estimateUsageCost(
      { promptTokens: 1_000_000, cachedPromptTokens: 2_000_000, completionTokens: 0 },
      { ...DEFAULT_COST_CONFIG, promptPerMillion: 1, cachedPromptPerMillion: 0.1 },
    );
    expect(cost).toBeCloseTo(0.1);
  });

  it('prices speech-to-text by audio minute and TTS by million characters', () => {
    expect(estimateUsageCost(
      { promptTokens: 0, completionTokens: 0, inputAudioSeconds: 600, inputCharacters: 0 },
      { ...DEFAULT_COST_CONFIG, billingUnit: 'audio_minute', pricePerUnit: 0.006 },
    )).toBeCloseTo(0.06);
    expect(estimateUsageCost(
      { promptTokens: 0, completionTokens: 0, inputAudioSeconds: 0, inputCharacters: 2_000_000 },
      { ...DEFAULT_COST_CONFIG, billingUnit: 'million_characters', pricePerUnit: 15 },
    )).toBeCloseTo(30);
  });

  it('estimates legacy zero-cache usage without overriding recorded or newer values', () => {
    const pricing = {
      ...DEFAULT_COST_CONFIG,
      promptPerMillion: 0.14,
      cachedPromptPerMillion: 0.05,
      outputPerMillion: 1,
      legacyCachedPromptRatio: 0.95,
      legacyCachedPromptBefore: '2026-08-18',
    };
    expect(estimateUsageCost(
      { model: 'qwen3.6-35b-a3b', bucket: '2026-08-17', promptTokens: 1_000_000, cachedPromptTokens: 0, completionTokens: 0 },
      pricing,
    )).toBeCloseTo(0.0545);
    expect(estimateUsageCost(
      { model: 'qwen3.6-35b-a3b', bucket: '2026-08-17', promptTokens: 1_000_000, cachedPromptTokens: 400_000, completionTokens: 0 },
      pricing,
    )).toBeCloseTo(0.104);
    expect(estimateUsageCost(
      { model: 'qwen3.6-35b-a3b', bucket: '2026-08-18', promptTokens: 1_000_000, cachedPromptTokens: 0, completionTokens: 0 },
      pricing,
    )).toBeCloseTo(0.14);
    expect(estimateUsageCost(
      { model: 'qwen3.6-35b-a3b', lastTimestampUnixMs: Date.UTC(2026, 7, 17, 23, 30), promptTokens: 1_000_000, cachedPromptTokens: 0, completionTokens: 0 },
      pricing,
    )).toBeCloseTo(0.0545);
  });
});

describe('normalizeCostConfig', () => {
  it('preserves user-edited prices over new defaults', () => {
    const next = normalizeCostConfig('m', {
      promptPerMillion: 9.99,
      cachedPromptPerMillion: 7.77,
      outputPerMillion: 8.88,
      userEdited: true,
      defaultsVersion: 1,
    }, { ...DEFAULT_COST_CONFIG, promptPerMillion: 0.1, cachedPromptPerMillion: 0.01, outputPerMillion: 0.2 });
    expect(next.promptPerMillion).toBe(9.99);
    expect(next.cachedPromptPerMillion).toBe(7.77);
    expect(next.outputPerMillion).toBe(8.88);
    expect(next.userEdited).toBe(true);
  });

  it('preserves zero as an intentionally disabled break-even target', () => {
    const next = normalizeCostConfig('m', { breakEvenTarget: 0 }, DEFAULT_COST_CONFIG);
    expect(next.breakEvenTarget).toBe(0);
  });
});

describe('estimatePortfolioCostAvoided', () => {
  it('sums per-model costs using each model\'s config', () => {
    const defaults = {
      a: { ...DEFAULT_COST_CONFIG, promptPerMillion: 1, outputPerMillion: 2 },
      b: { ...DEFAULT_COST_CONFIG, promptPerMillion: 10, outputPerMillion: 20 },
    };
    const total = estimatePortfolioCostAvoided(
      [usageRow('a', 1_000_000, 1_000_000), usageRow('b', 1_000_000, 1_000_000)],
      {},
      defaults,
      DEFAULT_COST_CONFIG,
    );
    expect(total).toBeCloseTo(3 + 30);
  });
});

describe('buildTokenSeries', () => {
  it('aggregates persisted monthly buckets for the all-time range', () => {
    const monthly: MonthlyUsageRow[] = [
      { bucket: '2026-05', model: 'a', promptTokens: 100, completionTokens: 50, totalTokens: 150, requests: 2, successfulRequests: 2 },
      { bucket: '2026-06', model: 'a', promptTokens: 200, completionTokens: 100, totalTokens: 300, requests: 3, successfulRequests: 3 },
      { bucket: '2026-06', model: 'b', promptTokens: 1000, completionTokens: 500, totalTokens: 1500, requests: 1, successfulRequests: 1 },
    ];
    const series = buildTokenSeries([], 'a', DEFAULT_COST_CONFIG, monthly, {}, {}, DEFAULT_COST_CONFIG, 'all');
    expect(series.months.length).toBe(2);
    expect(series.total).toEqual([150, 300]);
    expect(series.prompt).toEqual([100, 200]);

    const all = buildTokenSeries([], ALL_MODELS, DEFAULT_COST_CONFIG, monthly, {}, {}, DEFAULT_COST_CONFIG, 'all');
    expect(all.total).toEqual([150, 1800]);
    expect(tokenUsageFromSeries(ALL_MODELS, all).total).toBe(1950);
  });

  it('uses cached input pricing for persisted usage and request history', () => {
    const pricing = {
      ...DEFAULT_COST_CONFIG,
      promptPerMillion: 1,
      cachedPromptPerMillion: 0.1,
      outputPerMillion: 2,
    };
    const monthly: MonthlyUsageRow[] = [{
      bucket: '2026-06', model: 'a', promptTokens: 1_000_000,
      cachedPromptTokens: 250_000, completionTokens: 500_000,
      totalTokens: 1_500_000, requests: 1, successfulRequests: 1,
    }];
    const persisted = buildTokenSeries([], 'a', pricing, monthly, {}, {}, DEFAULT_COST_CONFIG, 'all');
    expect(persisted.cost.reduce((sum, value) => sum + value, 0)).toBeCloseTo(1.775);

    const now = Date.now();
    const jobs: JobRecord[] = [{
      id: 'cached', type: 'chat', status: 'succeeded', model: 'a',
      createdAt: new Date(now).toISOString(), timestampUnixMs: now,
      promptTokens: 1_000_000, cachedPromptTokens: 250_000,
      completionTokens: 500_000, totalTokens: 1_500_000,
      tokensPerSecond: 1, durationMs: 1000, httpStatus: 200, slotId: 0,
    }];
    const recent = buildTokenSeries(jobs, 'a', pricing, [], {}, {}, DEFAULT_COST_CONFIG, 'all');
    expect(recent.cachedPrompt.reduce((sum, value) => sum + value, 0)).toBe(250_000);
    expect(recent.cost.reduce((sum, value) => sum + value, 0)).toBeCloseTo(1.775);
  });

  it('uses daily rows to apply legacy cache estimates precisely in the yearly range', () => {
    const pricing = {
      ...DEFAULT_COST_CONFIG,
      promptPerMillion: 0.14,
      cachedPromptPerMillion: 0.05,
      legacyCachedPromptRatio: 0.95,
      legacyCachedPromptBefore: '2026-08-18',
    };
    const monthly: MonthlyUsageRow[] = [{
      bucket: '2026-08', model: 'qwen3.6-35b-a3b', promptTokens: 2_000_000,
      cachedPromptTokens: 0, completionTokens: 0, totalTokens: 2_000_000,
      requests: 2, successfulRequests: 2,
    }];
    const daily: MonthlyUsageRow[] = [
      { bucket: '2026-08-17', model: 'qwen3.6-35b-a3b', promptTokens: 1_000_000, cachedPromptTokens: 0, completionTokens: 0, totalTokens: 1_000_000, requests: 1, successfulRequests: 1 },
      { bucket: '2026-08-18', model: 'qwen3.6-35b-a3b', promptTokens: 1_000_000, cachedPromptTokens: 0, completionTokens: 0, totalTokens: 1_000_000, requests: 1, successfulRequests: 1 },
    ];
    const series = buildTokenSeries([], 'qwen3.6-35b-a3b', pricing, monthly, {}, {}, DEFAULT_COST_CONFIG, 'year', daily, [], true);
    expect(series.cachedPrompt.reduce((sum, value) => sum + value, 0)).toBe(950_000);
    expect(series.cost.reduce((sum, value) => sum + value, 0)).toBeCloseTo(0.1945);
  });

  it('keeps overview and usage all-time costs identical across long mixed-date history', () => {
    const pricing = {
      ...DEFAULT_COST_CONFIG,
      promptPerMillion: 0.14,
      cachedPromptPerMillion: 0.05,
      legacyCachedPromptRatio: 0.95,
      legacyCachedPromptBefore: '2026-08-18',
    };
    const monthly: MonthlyUsageRow[] = Array.from({ length: 13 }, (_, index) => ({
      bucket: index < 12
        ? `2025-${String(index + 1).padStart(2, '0')}`
        : '2026-01',
      model: 'qwen3.6-35b-a3b',
      promptTokens: index === 0 ? 3_000_000 : 1_000_000,
      cachedPromptTokens: 0,
      completionTokens: 0,
      totalTokens: index === 0 ? 3_000_000 : 1_000_000,
      requests: index === 0 ? 3 : 1,
      successfulRequests: index === 0 ? 3 : 1,
    }));
    const daily: MonthlyUsageRow[] = [
      ...Array.from({ length: 13 }, (_, index) => ({
        bucket: `2026-08-${String(index + 1).padStart(2, '0')}`,
        model: 'qwen3.6-35b-a3b',
        promptTokens: 1_000_000,
        cachedPromptTokens: 0,
        completionTokens: 0,
        totalTokens: 1_000_000,
        requests: 1,
        successfulRequests: 1,
      })),
      { bucket: '2026-08-18', model: 'qwen3.6-35b-a3b', promptTokens: 1_000_000, cachedPromptTokens: 0, completionTokens: 0, totalTokens: 1_000_000, requests: 1, successfulRequests: 1 },
      { bucket: '2026-08-19', model: 'qwen3.6-35b-a3b', promptTokens: 1_000_000, cachedPromptTokens: 0, completionTokens: 0, totalTokens: 1_000_000, requests: 1, successfulRequests: 1 },
    ];
    const defaults = { 'qwen3.6-35b-a3b': pricing };
    const overview = buildTokenSeries(
      [], ALL_MODELS, DEFAULT_COST_CONFIG, monthly, {}, defaults,
      DEFAULT_COST_CONFIG, 'all', daily, [], true,
    );
    const usage = buildTokenSeries(
      [], 'qwen3.6-35b-a3b', pricing, monthly, {}, defaults,
      DEFAULT_COST_CONFIG, 'all', daily, [], true,
    );
    const overviewCost = overview.cost.reduce((sum, value) => sum + value, 0);
    const usageCost = usage.cost.reduce((sum, value) => sum + value, 0);
    expect(overview.cachedPrompt.reduce((sum, value) => sum + value, 0)).toBe(12_350_000);
    expect(overviewCost).toBeCloseTo(0.9885);
    expect(overviewCost).toBe(usageCost);
  });

  it('keeps measured throughput samples separate from historical token totals', () => {
    const monthly: MonthlyUsageRow[] = [{
      bucket: '2026-06', model: 'a', promptTokens: 1_000_000, cachedPromptTokens: 0,
      completionTokens: 500_000, measuredPromptTokens: 600, measuredCompletionTokens: 80,
      totalTokens: 1_500_000, requests: 10, successfulRequests: 10,
      promptDurationMs: 1_000, generationDurationMs: 2_000,
      peakPromptTokensPerSecond: 600, peakTokensPerSecond: 50,
    }];
    const series = buildTokenSeries([], 'a', DEFAULT_COST_CONFIG, monthly, {}, {}, DEFAULT_COST_CONFIG, 'all');
    expect(series.measuredCompletionTokens).toEqual([80]);
    expect(series.measuredPromptTokens).toEqual([600]);
    expect(series.generationDurationMs).toEqual([2_000]);
    expect(series.peakTokensPerSecond).toEqual([50]);
  });

  it('excludes failed and unmeasured job history from throughput', () => {
    const now = Date.now();
    const base: JobRecord = {
      id: 'measured', type: 'chat', status: 'succeeded', model: 'a',
      createdAt: new Date(now).toISOString(), timestampUnixMs: now,
      promptTokens: 120, cachedPromptTokens: 20, completionTokens: 40, totalTokens: 160,
      tokensPerSecond: 20, promptTokensPerSecond: 100,
      generationDurationMs: 2_000, promptDurationMs: 1_000,
      durationMs: 3_000, httpStatus: 200, slotId: 0,
    };
    const jobs: JobRecord[] = [
      base,
      { ...base, id: 'legacy', completionTokens: 100_000, generationDurationMs: 0, promptDurationMs: 0 },
      { ...base, id: 'failed', status: 'failed', tokensPerSecond: 10_000, httpStatus: 500 },
    ];
    const series = buildTokenSeries(jobs, 'a', DEFAULT_COST_CONFIG, [], {}, {}, DEFAULT_COST_CONFIG, 'all');
    expect(series.measuredCompletionTokens.reduce((sum, value) => sum + value, 0)).toBe(40);
    expect(series.measuredPromptTokens.reduce((sum, value) => sum + value, 0)).toBe(100);
    expect(Math.max(...series.peakTokensPerSecond)).toBe(20);
  });

  it('uses request history when persisted rows belong only to another model', () => {
    const now = Date.now();
    const jobs: JobRecord[] = [{
      id: 'job-1',
      type: 'chat',
      status: 'succeeded',
      model: 'a',
      createdAt: new Date(now).toISOString(),
      timestampUnixMs: now,
      promptTokens: 40,
      completionTokens: 10,
      totalTokens: 50,
      tokensPerSecond: 1,
      durationMs: 1000,
      httpStatus: 200,
      slotId: 0,
    }];
    const monthly: MonthlyUsageRow[] = [{
      bucket: '2026-06',
      model: 'b',
      promptTokens: 100,
      completionTokens: 50,
      totalTokens: 150,
      requests: 1,
      successfulRequests: 1,
    }];
    const series = buildTokenSeries(jobs, 'a', DEFAULT_COST_CONFIG, monthly, {}, {}, DEFAULT_COST_CONFIG, 'all');
    expect(tokenUsageFromSeries('a', series).total).toBe(50);
  });

  it('uses finer daily resolution for all time when fewer than 12 monthly buckets exist', () => {
    const monthly: MonthlyUsageRow[] = [
      { bucket: '2026-06', model: 'a', promptTokens: 100, completionTokens: 50, totalTokens: 150, requests: 2, successfulRequests: 2 },
      { bucket: '2026-07', model: 'a', promptTokens: 200, completionTokens: 100, totalTokens: 300, requests: 3, successfulRequests: 3 },
    ];
    const daily: MonthlyUsageRow[] = Array.from({ length: 20 }, (_, index) => ({
      bucket: `2026-07-${String(index + 1).padStart(2, '0')}`,
      model: 'a',
      promptTokens: 10,
      completionTokens: 5,
      totalTokens: 15,
      requests: 1,
      successfulRequests: 1,
    }));
    const series = buildTokenSeries(
      [],
      'a',
      DEFAULT_COST_CONFIG,
      monthly,
      {},
      {},
      DEFAULT_COST_CONFIG,
      'all',
      daily,
      [],
      true,
    );
    expect(series.months.length).toBeGreaterThanOrEqual(12);
    expect(series.months.length).toBeLessThanOrEqual(24);
    expect(series.total.reduce((sum, value) => sum + value, 0)).toBe(300);
    expect(series.requests.reduce((sum, value) => sum + value, 0)).toBe(20);
  });
});

describe('getCostConfigForModel', () => {
  it('uses the model default when nothing is saved', () => {
    const defaults = { m: { ...DEFAULT_COST_CONFIG, promptPerMillion: 0.118 } };
    expect(getCostConfigForModel('m', {}, defaults).promptPerMillion).toBe(0.118);
    expect(getCostConfigForModel('unknown', {}, defaults).promptPerMillion).toBe(DEFAULT_COST_CONFIG.promptPerMillion);
    expect(getCostConfigForModel('whisper-base-en', {}, defaults).billingUnit).toBe('audio_minute');
    expect(getCostConfigForModel('windows-sapi', {}, defaults).billingUnit).toBe('million_characters');
  });
});
