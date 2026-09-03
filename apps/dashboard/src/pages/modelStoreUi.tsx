import React from 'react';
import type { InstalledStoreModel, StoreFile } from '../api';
import { Badge } from '../components/ui';
import type { DashboardSection } from '../dashboardSections';

export const storeInputClass = 'min-h-11 rounded border border-white/10 bg-[#07101d] px-3 text-sm text-text-primary';
export type StoreTab = 'discover' | 'downloads' | 'installed';
export type CatalogueSort = 'trending' | 'downloads' | 'likes' | 'recent';
export type ServerSortKey = 'name' | 'type' | 'configured' | 'runtime' | 'size';

export const storeScope: Record<DashboardSection, {
  runtime: string;
  modality: string;
  runtimeLabel: string;
}> = {
  llm: { runtime: 'llama_cpp', modality: 'text', runtimeLabel: 'llama.cpp / GGUF' },
  dictation: { runtime: 'whisper_cpp', modality: 'audio_transcription', runtimeLabel: 'whisper.cpp' },
  image: { runtime: 'stable_diffusion_cpp', modality: 'image', runtimeLabel: 'Stable Diffusion / stable-diffusion.cpp' },
  music: { runtime: 'ace_step_cpp', modality: 'audio_generation', runtimeLabel: 'ACE-Step C++' },
};

export const storeSearchPlaceholder: Record<DashboardSection, string> = {
  llm: 'Search language models',
  dictation: 'Search speech models',
  image: 'Search image generation models',
  music: 'Search music generation models',
};

export function serverModelType(entry: InstalledStoreModel): string {
  if (entry.hasVision) return 'Vision and text';
  if (entry.modality === 'audio_transcription') return 'Speech to text';
  if (entry.modality === 'audio_speech') return 'Text to speech';
  if (entry.modality === 'image') return 'Image generation';
  if (entry.modality === 'audio_generation') return 'Music generation';
  return entry.modality || 'Text';
}

export function defaultStoreModelName(
  file: Pick<StoreFile, 'repo' | 'name' | 'variant'>,
): string {
  const artifactName = file.variant || file.name;
  return `${file.repo.split('/').pop() || 'model'}-${artifactName.replace(/\.[^.]+$/, '').slice(-40)}`
    .replace(/[^A-Za-z0-9_.-]/g, '_');
}

export const ArtifactFit: React.FC<{
  section: DashboardSection;
  estimatedVramMb: number;
  vramTotalMb: number;
}> = ({ section, estimatedVramMb, vramTotalMb }) => {
  if (section === 'dictation') return <Badge label="CPU-friendly" tone="good" />;
  if (!vramTotalMb) {
    return <Badge label={`${estimatedVramMb.toLocaleString()} MB / verify profile`} tone="warn" />;
  }
  const share = estimatedVramMb / vramTotalMb;
  if (share <= 0.65) return <Badge label="Fits with headroom" tone="good" />;
  if (share <= 0.85 && section !== 'llm') {
    return <Badge label="Fits with limited headroom" tone="warn" />;
  }
  if (share <= 0.85) return <Badge label="Fits / tight at long context" tone="warn" />;
  return <Badge label="Not recommended for this VRAM" tone="critical" />;
};

export function estimateRepositoryVramMb(modelId: string): number {
  const matches = Array.from(modelId.matchAll(/(?:^|[-_.])(\d+(?:\.\d+)?)b(?:$|[-_.])/gi));
  const parametersBillions = matches.length
    ? Math.max(...matches.map(match => Number(match[1])))
    : 8;
  const normalized = modelId.toLowerCase();
  const bytesPerParameter = normalized.includes('q2') || normalized.includes('iq2') ? 0.38
    : normalized.includes('q3') || normalized.includes('iq3') ? 0.5
      : normalized.includes('q5') ? 0.75
        : normalized.includes('q6') ? 0.88
          : normalized.includes('q8') ? 1.1
            : normalized.includes('f16') || normalized.includes('bf16') ? 2.1
              : 0.65;
  return parametersBillions * 1024 * bytesPerParameter + 1024;
}
