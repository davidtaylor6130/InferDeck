import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { GatewayContext, type GatewayValue } from '../gateway';
import { OverviewPage } from './OverviewPage';
import { ModelsPage } from './ModelsPage';
import { UsagePage } from './UsagePage';
import { SystemPage } from './SystemPage';
import type { StatsEvent, StatusPayload } from '../types';

const stats: StatsEvent = {
  timestampUnixMs: Date.now(),
  gpu: { available: true, name: 'Test GPU', utilizationPct: 42, vramUsedMb: 8192, temperatureC: 61, powerW: 180 },
  loadedModel: 'qwen3.6-35b-a3b',
  activeRequests: 1,
  swapping: false,
  swapTarget: '',
  totalRequests: 12,
  totalSwaps: 3,
  lifetimeTokensIn: 384_220,
  lifetimeTokensOut: 120_000,
  avgTokensPerSecond: 41.5,
  uptimeSeconds: 8421,
};

const status: StatusPayload = {
  status: 'ok',
  queue: { running: 1, gpuLocked: true, lockOwner: 'qwen3.6-35b-a3b' },
  swap: { swapping: false, target: '', from: '', startedUnixMs: 0, lastError: '' },
  hardware: {
    memory: { used: 27 * 1024 ** 3, total: 32 * 1024 ** 3, percentage: 84 },
    cpu: { name: 'Test CPU', logicalProcessors: 24 },
  },
  summary: { totalRequests: 12, totalTokens: 504_220, promptTokens: 384_220, completionTokens: 120_000, avgLatencyMs: 900, p50LatencyMs: 750, p95LatencyMs: 2200 },
  metrics: { total_requests: 12, total_swaps: 3, total_tokens: 504_220, avg_tokens_per_second: 41.5 },
  tokenUsage: [{
    model: 'qwen3.6-35b-a3b', requests: 12, successfulRequests: 11,
    promptTokens: 384_220, completionTokens: 120_000, totalTokens: 504_220,
    peakTokensPerSecond: 55, avgTokensPerSecond: 41.5, lastTimestampUnixMs: Date.now(),
  }],
  monthlyTokenUsage: [{ bucket: '2026-06', model: 'qwen3.6-35b-a3b', promptTokens: 384_220, completionTokens: 120_000, totalTokens: 504_220, requests: 12, successfulRequests: 11 }],
  models: [],
  current: 'qwen3.6-35b-a3b',
  uptime: 8421,
};

const value: GatewayValue = {
  connection: 'connected',
  lastUpdatedAt: Date.now(),
  stats,
  statsHistory: [stats],
  status,
  models: [{ id: 'qwen3.6-35b-a3b', family: 'qwen3.6', context_size: 100_000, vram_required_mb: 22_000, n_slots: 2, has_vision: true, loaded: true }],
  swap: status.swap,
  activity: [],
  refresh: async () => {},
  swapTo: async () => null,
  cancelSwap: async () => null,
  unload: async () => null,
};

const renderWith = (node: React.ReactElement) =>
  renderToStaticMarkup(<GatewayContext.Provider value={value}>{node}</GatewayContext.Provider>);

describe('pages', () => {
  it('Overview shows the loaded model, live stats, and lifetime counters', () => {
    const html = renderWith(<OverviewPage />);
    expect(html).toContain('qwen3.6-35b-a3b');
    expect(html).toContain('GPU utilization');
    expect(html).toContain('42%');
    expect(html).toContain('Tokens in');
    expect(html).toContain('p95 latency');
    expect(html).toContain('Vision');
  });

  it('Models lists registered models with load state', () => {
    const html = renderWith(<ModelsPage />);
    expect(html).toContain('Registered models');
    expect(html).toContain('qwen3.6-35b-a3b');
    expect(html).toContain('Loaded');
    expect(html).toContain('Swap history');
  });

  it('Usage renders the cost panel and per-model table', () => {
    const html = renderWith(<UsagePage />);
    expect(html).toContain('Token usage &amp; cost');
    expect(html).toContain('Portfolio cost avoided');
    expect(html).toContain('Prompt $/1M');
    expect(html).toContain('Per-model usage');
  });

  it('System renders hardware meters and the log viewer', () => {
    const html = renderWith(<SystemPage />);
    expect(html).toContain('GPU');
    expect(html).toContain('System RAM');
    expect(html).toContain('Gateway log');
  });
});
