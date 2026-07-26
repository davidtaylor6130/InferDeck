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
      { model_name: 'default', prompt_price_per_million: 0.5, completion_price_per_million: 1.0 },
      { model_name: 'qwen3.6-35b-a3b', prompt_price_per_million: 0.118, completion_price_per_million: 1.05, equivalent_api_model: 'Equivalent API model' },
    ]);
    expect(fallback.promptPerMillion).toBe(0.5);
    expect(defaults['qwen3.6-35b-a3b'].outputPerMillion).toBe(1.05);
    expect(defaults['qwen3.6-35b-a3b'].equivalentModel).toBe('Equivalent API model');
  });
});

describe('estimateCostAvoided', () => {
  it('prices prompt and output tokens per million', () => {
    const cost = estimateCostAvoided(
      { model: 'm', prompt: 2_000_000, output: 1_000_000, total: 3_000_000 },
      { ...DEFAULT_COST_CONFIG, promptPerMillion: 0.1, outputPerMillion: 0.4 },
    );
    expect(cost).toBeCloseTo(0.6);
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
});

describe('normalizeCostConfig', () => {
  it('preserves user-edited prices over new defaults', () => {
    const next = normalizeCostConfig('m', {
      promptPerMillion: 9.99,
      outputPerMillion: 8.88,
      userEdited: true,
      defaultsVersion: 1,
    }, { ...DEFAULT_COST_CONFIG, promptPerMillion: 0.1, outputPerMillion: 0.2 });
    expect(next.promptPerMillion).toBe(9.99);
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
