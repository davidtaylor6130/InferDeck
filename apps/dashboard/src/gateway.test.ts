import { describe, expect, it } from 'vitest';
import { mergeDailyUsage } from './gateway';
import type { MonthlyUsageRow } from './types';

const row = (
  bucket: string, model: string, promptTokens: number,
): MonthlyUsageRow => ({
  bucket,
  model,
  promptTokens,
  cachedPromptTokens: 0,
  completionTokens: 0,
  totalTokens: promptTokens,
  requests: 1,
  successfulRequests: 1,
});

describe('mergeDailyUsage', () => {
  it('retains old history and replaces overlapping recent buckets', () => {
    const merged = mergeDailyUsage(
      [row('2024-01-01', 'model', 10), row('2026-08-20', 'model', 20)],
      [row('2026-08-20', 'model', 30), row('2026-08-20', 'other', 40)],
    );
    expect(merged.map(item => [item.bucket, item.model, item.promptTokens]))
      .toEqual([
        ['2024-01-01', 'model', 10],
        ['2026-08-20', 'model', 30],
        ['2026-08-20', 'other', 40],
      ]);
  });
});
