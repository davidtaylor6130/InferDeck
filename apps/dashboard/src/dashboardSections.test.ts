import { describe, expect, it } from 'vitest';
import {
  bucketUsageForSection,
  modelsForSection,
  usageForSection,
} from './dashboardSections';
import type { ModelInfo, MonthlyUsageRow, UsageRow } from './types';

const models: ModelInfo[] = [
  { id: 'qwen', modality: 'text', context_size: 100_000, vram_required_mb: 22_000, n_slots: 4, has_vision: false, loaded: true },
  { id: 'parakeet', modality: 'audio_transcription', context_size: 0, vram_required_mb: 0, n_slots: 1, has_vision: false, loaded: false },
  { id: 'supertonic', modality: 'audio_speech', context_size: 0, vram_required_mb: 0, n_slots: 1, has_vision: false, loaded: false },
  { id: 'stable-diffusion', modality: 'image', context_size: 0, vram_required_mb: 4096, n_slots: 1, has_vision: false, loaded: false },
  { id: 'ace-step', modality: 'audio_generation', context_size: 0, vram_required_mb: 8192, n_slots: 1, has_vision: false, loaded: false },
];

const usage = (model: string): UsageRow => ({
  model,
  requests: 10,
  successfulRequests: 9,
  promptTokens: model === 'qwen' ? 100 : 0,
  completionTokens: model === 'qwen' ? 50 : 0,
  totalTokens: model === 'qwen' ? 150 : 0,
  peakTokensPerSecond: 20,
  avgTokensPerSecond: 10,
  lastTimestampUnixMs: 123,
});

const bucket = (model: string): MonthlyUsageRow => ({
  bucket: '2026-07',
  model,
  promptTokens: model === 'qwen' ? 100 : 0,
  completionTokens: model === 'qwen' ? 50 : 0,
  totalTokens: model === 'qwen' ? 150 : 0,
  requests: 10,
  successfulRequests: 9,
});

describe('dashboard section data boundaries', () => {
  it('separates every division by declared modality', () => {
    expect(modelsForSection(models, 'llm').map(model => model.id)).toEqual(['qwen']);
    expect(modelsForSection(models, 'dictation').map(model => model.id)).toEqual(['parakeet', 'supertonic']);
    expect(modelsForSection(models, 'image').map(model => model.id)).toEqual(['stable-diffusion']);
    expect(modelsForSection(models, 'music').map(model => model.id)).toEqual(['ace-step']);
  });

  it('uses the same split for persisted lifetime and bucketed usage', () => {
    const rows = [usage('qwen'), usage('parakeet'), usage('supertonic'), usage('stable-diffusion'), usage('ace-step')];
    const buckets = [bucket('qwen'), bucket('parakeet'), bucket('supertonic'), bucket('stable-diffusion'), bucket('ace-step')];

    expect(usageForSection(rows, models, 'llm').map(row => row.model)).toEqual(['qwen']);
    expect(usageForSection(rows, models, 'dictation').map(row => row.model)).toEqual(['parakeet', 'supertonic']);
    expect(usageForSection(rows, models, 'image').map(row => row.model)).toEqual(['stable-diffusion']);
    expect(usageForSection(rows, models, 'music').map(row => row.model)).toEqual(['ace-step']);
    expect(bucketUsageForSection(buckets, models, 'dictation').map(row => row.model)).toEqual(['parakeet', 'supertonic']);
    expect(bucketUsageForSection(buckets, models, 'image').map(row => row.model)).toEqual(['stable-diffusion']);
    expect(bucketUsageForSection(buckets, models, 'music').map(row => row.model)).toEqual(['ace-step']);
  });

  it('keeps retired LLMs in LLM usage and historical SAPI or Whisper in Dictation', () => {
    expect(usageForSection([usage('retired-model')], models, 'llm')).toHaveLength(1);
    expect(usageForSection([usage('retired-model')], models, 'dictation')).toHaveLength(0);
    expect(usageForSection([usage('whisper-base-en')], models, 'llm')).toHaveLength(0);
    expect(usageForSection([usage('whisper-base-en')], models, 'dictation')).toHaveLength(1);
    expect(usageForSection([usage('windows-sapi')], models, 'dictation')).toHaveLength(1);
    expect(usageForSection([usage('stable-diffusion-xl')], models, 'image')).toHaveLength(1);
    expect(usageForSection([usage('flux-dev')], models, 'image')).toHaveLength(1);
    expect(usageForSection([usage('ace-step-v1.5')], models, 'music')).toHaveLength(1);
    expect(usageForSection([usage('musicgen-small')], models, 'music')).toHaveLength(1);
    expect(usageForSection([usage('stable-diffusion-xl')], models, 'llm')).toHaveLength(0);
    expect(usageForSection([usage('ace-step-v1.5')], models, 'llm')).toHaveLength(0);
  });
});
