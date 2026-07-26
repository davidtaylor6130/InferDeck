import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { parseDocument } from 'yaml';
import { GatewayContext, type GatewayValue } from '../gateway';
import { OverviewPage } from './OverviewPage';
import { ModelsPage } from './ModelsPage';
import { OperatePage, stageProfileOptimization } from './OperatePage';
import type { ProfileOptimizationCandidate } from '../api';
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
  totalRequests: 19,
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
  summary: { totalRequests: 19, totalTokens: 504_220, promptTokens: 384_220, completionTokens: 120_000, avgLatencyMs: 900, p50LatencyMs: 750, p95LatencyMs: 2200 },
  metrics: { total_requests: 19, total_swaps: 3, total_tokens: 504_220, avg_tokens_per_second: 41.5 },
  tokenUsage: [
    {
      model: 'qwen3.6-35b-a3b', requests: 12, successfulRequests: 11,
      promptTokens: 384_220, completionTokens: 120_000, totalTokens: 504_220,
      peakTokensPerSecond: 55, avgTokensPerSecond: 41.5, lastTimestampUnixMs: Date.now(),
    },
    {
      model: 'parakeet-tdt-0.6b-v3', requests: 7, successfulRequests: 7,
      promptTokens: 0, completionTokens: 0, totalTokens: 0,
      peakTokensPerSecond: 0, avgTokensPerSecond: 0, lastTimestampUnixMs: Date.now(),
      inputAudioSeconds: 3_600, inputCharacters: 0,
    },
  ],
  monthlyTokenUsage: [
    { bucket: '2026-06', model: 'qwen3.6-35b-a3b', promptTokens: 384_220, completionTokens: 120_000, totalTokens: 504_220, requests: 12, successfulRequests: 11 },
    { bucket: '2026-06', model: 'parakeet-tdt-0.6b-v3', promptTokens: 0, completionTokens: 0, totalTokens: 0, requests: 7, successfulRequests: 7, inputAudioSeconds: 3_600, inputCharacters: 0 },
  ],
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
  models: [
    { id: 'qwen3.6-35b-a3b', family: 'qwen3.6', modality: 'text', context_size: 100_000, vram_required_mb: 22_000, n_slots: 2, has_vision: true, loaded: true },
    { id: 'parakeet-tdt-0.6b-v3', family: 'parakeet', runtime: 'sherpa_onnx', runtime_available: true, modality: 'audio_transcription', context_size: 0, vram_required_mb: 0, n_slots: 1, has_vision: false, loaded: true },
    { id: 'supertonic-3', family: 'supertonic', runtime: 'sherpa_onnx', runtime_available: true, modality: 'audio_speech', context_size: 0, vram_required_mb: 0, n_slots: 1, has_vision: false, loaded: false },
  ],
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
  it('Home shows global LLM and dictation summaries with aggregate counters', () => {
    const html = renderWith(<OverviewPage />);
    expect(html).toContain('Everything at a glance');
    expect(html).toContain('LLM');
    expect(html).toContain('Dictation');
    expect(html).toContain('Open LLM');
    expect(html).toContain('Open Dictation');
    expect(html).toContain('GPU utilization');
    expect(html).toContain('42%');
    expect(html).toContain('Tokens in');
    expect(html).toContain('p95 latency');
    expect(html).toContain('Combined usage');
    expect(html).toContain('API-equivalent value');
  });

  it('LLM and dictation operate pages stay administration-only', () => {
    const llm = renderWith(<OperatePage section="llm" />);
    const dictation = renderWith(<OperatePage section="dictation" />);
    expect(llm).toContain('LLM operation');
    expect(llm).toContain('qwen3.6-35b-a3b');
    expect(dictation).toContain('Dictation operation');
    expect(dictation).toContain('parakeet-tdt-0.6b-v3');
    expect(dictation).toContain('Recording and playback stay in clients');
    expect(dictation).not.toContain('microphone');
    expect(llm).toContain('Model details');
    expect(llm).toContain('Auto-optimize with benchmark');
    expect(llm).toContain('Saving applies the active profile automatically');
    expect(llm).not.toContain('next restart');
  });

  it('stages every optimized runtime value in the selected model profile', () => {
    const candidate: ProfileOptimizationCandidate = {
      contextPerSlot: 100_000,
      slots: 4,
      nBatch: 2048,
      nUbatch: 1024,
      cacheTypeK: 'q4_0',
      cacheTypeV: 'q8_0',
      flashAttention: 'auto',
      estimatedVramMb: 24_000,
      reserveVramMb: 8_000,
      qualityScore: 0.98,
      speedScore: 0.94,
      parallelismScore: 1,
      headroomScore: 0.95,
      overallScore: 0.97,
      fits: true,
      reasons: [],
    };
    const updated = parseDocument(stageProfileOptimization(
      'gateway:\n  n_batch: 512\n  n_ubatch: 512\n  cache_type_k: q8_0\n  cache_type_v: q8_0\n  flash_attn: off\nmodel_registry:\n  - name: qwen3.6-27b\n    context_size: 40960\n    n_slots: 1\n',
      'qwen3.6-27b',
      candidate,
    )).toJS() as {
      gateway: Record<string, unknown>;
      model_registry: Array<Record<string, unknown>>;
    };

    expect(updated.model_registry[0]).toMatchObject({
      context_size: 100_000,
      n_slots: 4,
      n_batch: 2048,
      n_ubatch: 1024,
      cache_type_k: 'q4_0',
      cache_type_v: 'q8_0',
    });
    expect(updated.gateway).toMatchObject({
      flash_attn: 'auto',
    });
  });

  it('Models pages are section-specific catalogues rather than runtime controls', () => {
    const llm = renderWith(<ModelsPage section="llm" />);
    const dictation = renderWith(<ModelsPage section="dictation" />);
    expect(llm).toContain('LLM model catalogue');
    expect(llm).toContain('Recommended 20–40B');
    expect(llm).toContain('Recommended only');
    expect(llm).toContain('Models on this server');
    expect(dictation).toContain('Dictation model catalogue');
    expect(dictation).toContain('Speech to text');
    expect(dictation).toContain('Text to speech');
    expect(dictation).toContain('complete runtime bundles');
    expect(llm).not.toContain('Load history');
    expect(dictation).not.toContain('Load history');
  });

  it('Usage pages expose correctly scoped LLM and dictation economics', () => {
    const llm = renderWith(<UsagePage section="llm" />);
    const dictation = renderWith(<UsagePage section="dictation" />);
    expect(llm).toContain('LLM usage');
    expect(llm).toContain('Estimated API cost');
    expect(llm).toContain('Prompt $/1M');
    expect(llm).toContain('Usage time range');
    expect(llm).not.toContain('ROI remaining');
    expect(llm).not.toContain('Portfolio break-even $');
    expect(dictation).toContain('Dictation usage');
    expect(dictation).toContain('persisted SQL ledger');
    expect(dictation).toContain('Audio transcribed');
    expect(dictation).toContain('Estimated API cost');
    expect(dictation).toContain('OpenAI whisper-1');
    expect(dictation).not.toContain('ROI remaining');
    expect(dictation).not.toContain('Portfolio break-even USD');
  });

  it('Diagnostics pages expose section-specific runtime health', () => {
    const llm = renderWith(<SystemPage section="llm" />);
    const dictation = renderWith(<SystemPage section="dictation" />);
    expect(llm).toContain('LLM accelerator');
    expect(llm).toContain('GPU utilization');
    expect(dictation).toContain('Dictation runtimes');
    expect(dictation).toContain('parakeet-tdt-0.6b-v3');
    expect(dictation).toContain('Dictation host resources');
    expect(dictation).toContain('Configuration recovery');
    expect(dictation).toContain('Complete stable YAML');
    expect(dictation).toContain('Advanced dictation diagnostics');
    expect(dictation).toContain('collapsed for safety');
  });
});
