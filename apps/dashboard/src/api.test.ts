import { afterEach, describe, expect, it, vi } from 'vitest';
import { getModels, optimizeProfile, waitForActiveConfig, waitForStableConfig } from './api';
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
      object: 'list',
      data: [{
        id: 'qwen3.6-27b',
        family: 'qwen3.6',
        runtime: 'llama_cpp',
        runtime_available: true,
        modality: 'text',
        capabilities: ['chat'],
        context_size: 65_536,
        vram_required_mb: 24_000,
        n_slots: 4,
        has_vision: false,
        loaded: true,
        inferdeck: {
          residency: {
            loaded: true,
            primary: true,
            slots: 2,
            free_slots: 1,
            active_requests: 1,
            estimated_vram_mb: 23_500,
            resizing: true,
          },
        },
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
    });
  });

  it('keeps configured slots and registry metadata for an unloaded entry', async () => {
    respondWith({
      data: [{
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
        inferdeck: {
          residency: {
            loaded: false,
            primary: false,
            slots: 0,
            free_slots: 0,
            active_requests: 0,
            resizing: false,
          },
        },
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
      '/api/optimize/profile',
      expect.objectContaining({
        method: 'POST',
        body: expect.stringContaining('"contextPerSlot":100000'),
      }),
    );
  });
});
