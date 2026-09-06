import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  ApiError, authenticateDashboard, isAuthenticationError, logoutDashboard, cancelProfileBenchmark, createApiKey,
  generateImages, generateMusic, getApiKeys, getApiSettings, getModels, getMediaJobs, getStoreActivity,
  getProfileBenchmark, optimizeProfile, saveApiSettings, searchStore,
  startProfileBenchmark,
  updateApiKey,
  waitForActiveConfig, waitForStableConfig,
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

describe('searchStore', () => {
  it('sends the selected browse order, modality, runtime, and gated policy', async () => {
    respondWith({ models: [] });

    await searchStore(
      '', 'stable_diffusion_cpp', 'image', 50, 'trending', false,
    );

    const fetchMock = vi.mocked(fetch);
    const url = String(fetchMock.mock.calls[0]?.[0]);
    expect(url).toContain('/api/inferdeck/v1/model-store/search?');
    expect(url).toContain('q=');
    expect(url).toContain('runtime=stable_diffusion_cpp');
    expect(url).toContain('modality=image');
    expect(url).toContain('sort=trending');
    expect(url).toContain('includeGated=false');
    expect(url).toContain('limit=50');
  });
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

describe('dashboard authentication', () => {
  it('requests a remembered HTTP-only dashboard session', async () => {
    const fetchMock = vi.fn(async (
      _input: RequestInfo | URL,
      _init?: RequestInit,
    ) => ({
      ok: true,
      status: 200,
      json: async () => ({ ok: true }),
    }));
    vi.stubGlobal('fetch', fetchMock);

    await authenticateDashboard('local-network-secret');

    expect(fetchMock).toHaveBeenCalledWith(
      '/api/inferdeck/v1/dashboard/session',
      expect.objectContaining({
        method: 'POST',
        body: JSON.stringify({ token: 'local-network-secret', remember: true }),
      }),
    );
  });
});

describe('API access settings', () => {
  it('uses typed control routes for public access and managed keys', async () => {
    const responses = [
      {
        allowPublicTraffic: false,
        runningAllowPublicTraffic: false,
        publicPriority: -999999,
        activeRevision: 'rev-a',
        restartRequired: false,
      },
      {
        ok: true,
        allowPublicTraffic: true,
        runningAllowPublicTraffic: false,
        publicPriority: -999999,
        activeRevision: 'rev-b',
        restartRequired: false,
        applyScheduled: true,
      },
      { apiKeys: [] },
      {
        id: 'a'.repeat(32),
        name: 'worker',
        prefix: 'idk_1234',
        priority: -20,
        createdAtUnixMs: 1,
        updatedAtUnixMs: 1,
        revokedAtUnixMs: null,
        key: 'one-time-key',
      },
      {
        id: 'a'.repeat(32),
        name: 'worker',
        prefix: 'idk_1234',
        priority: 40,
        createdAtUnixMs: 1,
        updatedAtUnixMs: 2,
        revokedAtUnixMs: null,
      },
    ];
    const fetchMock = vi.fn(async (
      _input: RequestInfo | URL,
      _init?: RequestInit,
    ) => ({
      ok: true,
      status: 200,
      json: async () => responses.shift(),
    }));
    vi.stubGlobal('fetch', fetchMock);

    expect((await getApiSettings()).publicPriority).toBe(-999999);
    expect((await saveApiSettings(true, 'rev-a')).activeRevision)
      .toBe('rev-b');
    expect(await getApiKeys()).toEqual([]);
    expect((await createApiKey('worker', -20)).key).toBe('one-time-key');
    expect((await updateApiKey('a'.repeat(32), 'worker', 40)).priority)
      .toBe(40);

    expect(fetchMock.mock.calls[0]?.[0])
      .toContain('/api/inferdeck/v1/api-settings');
    expect(fetchMock.mock.calls[1]?.[1]).toMatchObject({
      method: 'PUT',
      body: JSON.stringify({
        allowPublicTraffic: true,
        revision: 'rev-a',
      }),
    });
    expect(fetchMock.mock.calls[3]?.[1]).toMatchObject({
      method: 'POST',
      body: JSON.stringify({ name: 'worker', priority: -20 }),
    });
    expect(fetchMock.mock.calls[4]?.[1]).toMatchObject({
      method: 'PATCH',
      body: JSON.stringify({ name: 'worker', priority: 40 }),
    });
  });
});

describe('dashboard media generation', () => {
  it('uses control-session routes and returns image and music job metadata', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce({
        ok: true,
        status: 200,
        headers: new Headers({ 'X-InferDeck-Job-Id': '41' }),
        json: async () => ({
          created: 1,
          output_format: 'png',
          data: [{ b64_json: 'iVBORw==' }],
        }),
      })
      .mockResolvedValueOnce({
        ok: true,
        status: 200,
        headers: new Headers({
          'X-InferDeck-Job-Id': '42',
          'X-InferDeck-Seed': '1234',
          'X-InferDeck-Audio-Duration-Seconds': '10',
        }),
        blob: async () => new Blob(['RIFF'], { type: 'audio/wav' }),
      });
    vi.stubGlobal('fetch', fetchMock);

    const image = await generateImages({
      model: 'stable-diffusion',
      prompt: 'a lighthouse',
      size: '512x512',
      n: 1,
    });
    const music = await generateMusic({
      model: 'ace-step',
      prompt: 'warm analogue synths',
      lyrics: '',
      duration: 10,
      seed: 1234,
      steps: 0,
      guidance_scale: 0,
    });

    expect(image.jobId).toBe(41);
    expect(music.jobId).toBe(42);
    expect(music.seed).toBe(1234);
    expect(music.audio.type).toBe('audio/wav');
    expect(fetchMock).toHaveBeenNthCalledWith(
      1,
      '/api/inferdeck/v1/media/images/generations',
      expect.objectContaining({
        method: 'POST',
        body: expect.stringContaining('"prompt":"a lighthouse"'),
      }),
    );
    expect(fetchMock).toHaveBeenNthCalledWith(
      2,
      '/api/inferdeck/v1/media/audio/generations',
      expect.objectContaining({
        method: 'POST',
        body: expect.stringContaining('"duration":10'),
      }),
    );
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

describe('polling request cancellation', () => {
  it.each([getMediaJobs, getStoreActivity])('forwards caller cancellation and retains the timeout', async load => {
    respondWith({ jobs: [], downloads: [], installed: {}, library: [] });
    const timeout = new AbortController();
    const timeoutSpy = vi.spyOn(AbortSignal, 'timeout').mockReturnValue(timeout.signal);
    try {
      const caller = new AbortController();
      await load(caller.signal);
      const signal = vi.mocked(fetch).mock.calls[0]?.[1]?.signal;
      expect(signal?.aborted).toBe(false);
      caller.abort();
      expect(signal?.aborted).toBe(true);
      await load(new AbortController().signal);
      const timedSignal = vi.mocked(fetch).mock.calls[1]?.[1]?.signal;
      expect(timedSignal?.aborted).toBe(false);
      timeout.abort();
      expect(timedSignal?.aborted).toBe(true);
      expect(timeoutSpy).toHaveBeenCalledWith(15_000);
    } finally {
      timeoutSpy.mockRestore();
    }
  });
});


describe('dashboard session controls', () => {
  it('honours session-only login without persisting the key client-side', async () => {
    respondWith({});
    await authenticateDashboard('test-only-key', false);
    expect(JSON.parse(String(vi.mocked(fetch).mock.calls[0]?.[1]?.body)))
      .toEqual({ token: 'test-only-key', remember: false });
  });
  it('logs out through the control session endpoint', async () => {
    respondWith({});
    await logoutDashboard();
    expect(vi.mocked(fetch).mock.calls[0]?.[0]).toBe('/api/inferdeck/v1/dashboard/session');
    expect(vi.mocked(fetch).mock.calls[0]?.[1]?.method).toBe('DELETE');
  });
  it('distinguishes rejected access from a network or server failure', () => {
    expect(isAuthenticationError(new ApiError(401, 'unauthorized'))).toBe(true);
    expect(isAuthenticationError(new ApiError(403, 'forbidden'))).toBe(true);
    expect(isAuthenticationError(new ApiError(503, 'unavailable'))).toBe(false);
    expect(isAuthenticationError(new TypeError('Failed to fetch'))).toBe(false);
  });
});
