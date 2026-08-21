import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  cancelProfileBenchmark, getModels, getProfileBenchmark, optimizeProfile,
  startProfileBenchmark, waitForActiveConfig, waitForStableConfig,
} from './api';
import type { ModelInfo } from './types';

function respondWith(body: unknown) {
  vi.stubGlobal('fetch', vi.fn(async () => ({
    ok: true,
    status: 200,
    json: async () => body,
  })));
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('getModels', () => {
  it('normalizes loaded residency fields while preserving registry metadata', async () => {
    respondWith({
      models: [{
        id: 'qwen3.6-27b',
        family: 'qwen3.6',
        runtime: 'llama_cpp',
        runtime_available: true,
        modality: 'text',
        capabilities: ['chat'],
        context_size: 65_536,
        vram_required_mb: 24_000,
        n_slots: 2,
        has_vision: false,
        loaded: true,
        optimization: {
          status: 'measured',
          measured_at: '2026-07-26',
          quality_passes: 3,
          quality_total: 3,
          single_tokens_per_second: 50.16,
          parallel_tokens_per_second: 51.24,
        },
        primary: true,
        free_slots: 1,
        active_requests: 1,
        resizing: true,
      }],
    });

    const [model] = await getModels();

    expect(model).toMatchObject({
      id: 'qwen3.6-27b',
      family: 'qwen3.6',
      runtime: 'llama_cpp',
      runtime_available: true,
      modality: 'text',
      capabilities: ['chat'],
      context_size: 65_536,
      vram_required_mb: 24_000,
      n_slots: 2,
      has_vision: false,
      loaded: true,
      primary: true,
      free_slots: 1,
      active_requests: 1,
      resizing: true,
      optimization: {
        status: 'measured',
        measured_at: '2026-07-26',
        quality_passes: 3,
        quality_total: 3,
        single_tokens_per_second: 50.16,
        parallel_tokens_per_second: 51.24,
      },
    });
  });

  it('keeps configured slots and registry metadata for an unloaded entry', async () => {
    respondWith({
      models: [{
        id: 'whisper-base-en',
        runtime: 'whisper_cpp',
        runtime_available: true,
        modality: 'audio_transcription',
        capabilities: ['audio_transcription'],
        context_size: 0,
        vram_required_mb: 512,
        n_slots: 3,
        has_vision: false,
        loaded: false,
        primary: false,
        free_slots: 0,
        active_requests: 0,
        resizing: false,
      }],
    });

    const [model] = await getModels();
    const withResizing = model as ModelInfo & { resizing?: boolean };

    expect(model).toMatchObject({
      id: 'whisper-base-en',
      runtime: 'whisper_cpp',
      runtime_available: true,
      modality: 'audio_transcription',
      capabilities: ['audio_transcription'],
      context_size: 0,
      vram_required_mb: 512,
      n_slots: 3,
      has_vision: false,
      loaded: false,
      primary: false,
      free_slots: 0,
      active_requests: 0,
    });
    expect(withResizing.resizing).toBe(false);
  });
});

describe('configuration apply recovery', () => {
  it('waits for the gateway runtime revision, not merely the saved file revision', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce({
        ok: true,
        status: 200,
        json: async () => ({
          activeRevision: 'new-revision',
          runningRevision: 'old-revision',
          revision: 'base-revision',
          hasActiveProfile: true,
          usingActiveProfile: true,
          fallbackReason: '',
        }),
      })
      .mockResolvedValueOnce({
        ok: true,
        status: 200,
        json: async () => ({
          activeRevision: 'new-revision',
          runningRevision: 'new-revision',
          revision: 'base-revision',
          hasActiveProfile: true,
          usingActiveProfile: true,
          fallbackReason: '',
        }),
      });
    vi.stubGlobal('fetch', fetchMock);

    const applied = await waitForActiveConfig('new-revision', 1_000, 1);

    expect(applied.runningRevision).toBe('new-revision');
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it('detects the stable baseline after an automatic reset reload', async () => {
    respondWith({
      activeRevision: 'base-revision',
      runningRevision: 'base-revision',
      revision: 'base-revision',
      hasActiveProfile: false,
      usingActiveProfile: false,
      fallbackReason: '',
    });

    const applied = await waitForStableConfig(1_000, 1);

    expect(applied.hasActiveProfile).toBe(false);
    expect(applied.runningRevision).toBe(applied.revision);
  });

  it('reports a rejected active profile immediately', async () => {
    respondWith({
      activeRevision: 'bad-revision',
      runningRevision: 'base-revision',
      revision: 'base-revision',
      hasActiveProfile: true,
      usingActiveProfile: false,
      fallbackReason: 'invalid slot bounds',
    });

    await expect(waitForActiveConfig('bad-revision', 1_000, 1))
      .rejects.toThrow('InferDeck rejected the saved profile: invalid slot bounds');
  });
});

describe('profile optimization', () => {
  it('posts explicit per-slot and runtime settings without applying them', async () => {
    respondWith({
      model: 'qwen3.6-27b',
      mode: 'profile_estimate',
      measured: false,
      observedTokensPerSecond: 35,
      modelFileMb: 17_000,
      totalVramMb: 32_768,
      weights: { quality: 0.6, speed: 0.15, parallelism: 0.15, headroom: 0.1 },
      recommended: {
        contextPerSlot: 100_000,
        slots: 4,
        nBatch: 2048,
        nUbatch: 2048,
        cacheTypeK: 'q4_0',
        cacheTypeV: 'q8_0',
        flashAttention: 'auto',
        estimatedVramMb: 24_000,
        reserveVramMb: 8_768,
        qualityScore: 0.98,
        speedScore: 0.95,
        parallelismScore: 1,
        headroomScore: 1,
        overallScore: 0.98,
        fits: true,
        reasons: [],
      },
      candidates: [],
      notes: [],
    });

    const result = await optimizeProfile({
      model: 'qwen3.6-27b',
      contextPerSlot: 100_000,
      slots: 4,
      minSlots: 1,
      nBatch: 2048,
      nUbatch: 2048,
      cacheTypeK: 'q4_0',
      cacheTypeV: 'q8_0',
    });

    expect(result.measured).toBe(false);
    expect(result.recommended.contextPerSlot).toBe(100_000);
    const fetchMock = vi.mocked(fetch);
    expect(fetchMock).toHaveBeenCalledWith(
      '/api/inferdeck/v1/optimize/profile',
      expect.objectContaining({
        method: 'POST',
        body: expect.stringContaining('"contextPerSlot":100000'),
      }),
    );
  });

  it('starts, polls, and cancels the measured benchmark API', async () => {
    respondWith({
      id: 7,
      state: 'running',
      stage: 'quality',
      message: 'Running measured probes',
      model: 'qwen3.6-27b',
      completedCandidates: 0,
      totalCandidates: 3,
      progressPct: 10,
      startedUnixMs: 1,
      finishedUnixMs: 0,
      measured: true,
      cancelRequested: false,
      restored: false,
      weights: { promptProcessing: 0.5, generation: 0.5 },
      baseline: null,
      recommended: null,
      candidates: [],
    });

    const started = await startProfileBenchmark({
      model: 'qwen3.6-27b',
      contextPerSlot: 100_000,
      slots: 4,
      minSlots: 1,
      nBatch: 2048,
      nUbatch: 2048,
      cacheTypeK: 'q4_0',
      cacheTypeV: 'q8_0',
      candidateLimit: 3,
    });
    await getProfileBenchmark();
    await cancelProfileBenchmark();

    expect(started.measured).toBe(true);
    const fetchMock = vi.mocked(fetch);
    expect(fetchMock).toHaveBeenNthCalledWith(
      1,
      '/api/inferdeck/v1/optimize/benchmark',
      expect.objectContaining({
        method: 'POST',
        body: expect.stringContaining('"candidateLimit":3'),
      }),
    );
    expect(fetchMock).toHaveBeenNthCalledWith(
      2,
      '/api/inferdeck/v1/optimize/benchmark',
      expect.objectContaining({ headers: { Accept: 'application/json' } }),
    );
    expect(fetchMock).toHaveBeenNthCalledWith(
      3,
      '/api/inferdeck/v1/optimize/benchmark/cancel',
      expect.objectContaining({ method: 'POST' }),
    );
  });
});
