import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import type { JobRecord } from '../types';
import { RequestDetails, requestMeasurements } from './RequestsPage';
const job: JobRecord = {
  id: 'request-1', type: '/v1/chat/completions', status: 'succeeded', model: 'test-model', modality: 'text',
  createdAt: '', timestampUnixMs: 1, promptTokens: 1000, cachedPromptTokens: 900, cacheWriteTokens: 100, completionTokens: 100,
  totalTokens: 1100, tokensPerSecond: 25, promptTokensPerSecond: 500, generationDurationMs: 4000,
  promptDurationMs: 200, queueDurationMs: 0, swapLoadDurationMs: 0, durationMs: 4200, httpStatus: 200, slotId: 0,
};
describe('request measurements', () => {
  it('shows individual generation rate and actual prompt reuse', () => {
    expect(requestMeasurements(job)).toMatchObject({ outputRate: 25, promptRate: 500, cachePercent: 90 });
    const html = renderToStaticMarkup(<RequestDetails job={job} />);
    expect(html).toContain('900 / 1,000 tokens (90.0%)');
    expect(html).toContain('25.0 tok/s');
    expect(html).toContain('Matching prompt prefix reused.');
  });
  it('keeps absent, invalid and non-text measurements unknown', () => {
    const missing = { ...job, cachedPromptTokens: undefined, generationDurationMs: undefined, promptDurationMs: undefined };
    expect(requestMeasurements(missing)).toMatchObject({ outputRate: undefined, promptRate: undefined, cachePercent: undefined });
    expect(requestMeasurements({ ...job, cachedPromptTokens: 1001 }).cachePercent).toBeUndefined();
    expect(requestMeasurements({ ...job, modality: 'image' }).outputRate).toBeUndefined();
    expect(requestMeasurements({ ...job, tokensPerSecond: Number.NaN }).outputRate).toBeUndefined();
    expect(requestMeasurements({ ...job, cachedPromptTokens: 0, cacheWriteTokens: 0 }).cacheReason).toBe('Cache reuse was not measured.');
    const historical = renderToStaticMarkup(<RequestDetails job={{ ...job, queueDurationMs: 0, swapLoadDurationMs: 0, firstTokenDurationMs: 0 }} />);
    expect(historical).toContain('Not recorded');
  });
  it('reports a measured cache miss without inventing its cause', () => {
    const missed = { ...job, cachedPromptTokens: 0, cacheWriteTokens: 1000, status: 'failed' as const, httpStatus: 499 };
    const html = renderToStaticMarkup(<RequestDetails job={missed} />);
    expect(requestMeasurements(missed).cachePercent).toBe(0);
    expect(html).toContain('Prompt processed without cache reuse.');
    expect(html).toContain('Client disconnected or cancelled.');
    expect(html).not.toContain('model swap');
  });
});
