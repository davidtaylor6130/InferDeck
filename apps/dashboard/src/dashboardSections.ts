import type { ModelInfo, MonthlyUsageRow, UsageRow } from './types';

export const DASHBOARD_SECTIONS = ['llm', 'dictation', 'image', 'music'] as const;
export type DashboardSection = typeof DASHBOARD_SECTIONS[number];

const DICTATION_MODALITIES = new Set(['audio_transcription', 'audio_speech']);
const DICTATION_MODEL_NAME = /(?:^|[-_.])(sapi|whisper|parakeet|supertonic|speech|tts|stt|asr)(?:$|[-_.])/i;
const IMAGE_MODEL_NAME = /(?:^|[-_.])(stable[-_.]?diffusion|sdxl|flux|z[-_.]?image|imagegen)(?:$|[-_.])/i;
const MUSIC_MODEL_NAME = /(?:^|[-_.])(ace[-_.]?step|musicgen|audiocraft|audio[-_.]?generation)(?:$|[-_.])/i;

export function isDictationModel(model: Pick<ModelInfo, 'modality'> | undefined): boolean {
  return DICTATION_MODALITIES.has(model?.modality ?? '');
}

export function modelBelongsToSection(
  model: Pick<ModelInfo, 'modality'> | undefined,
  section: DashboardSection,
): boolean {
  return sectionForModality(model?.modality) === section;
}

export function sectionForModality(modality?: string): DashboardSection {
  if (DICTATION_MODALITIES.has(modality ?? '')) return 'dictation';
  if (modality === 'image') return 'image';
  if (modality === 'audio_generation') return 'music';
  return 'llm';
}

export function modelsForSection(models: ModelInfo[], section: DashboardSection): ModelInfo[] {
  return models.filter(model => modelBelongsToSection(model, section));
}

export function modelIdsForSection(models: ModelInfo[], section: DashboardSection): Set<string> {
  return new Set(modelsForSection(models, section).map(model => model.id));
}

function usageBelongsToSection(
  model: string,
  models: ModelInfo[],
  section: DashboardSection,
): boolean {
  const info = models.find(candidate => candidate.id === model);
  return info
    ? modelBelongsToSection(info, section)
    : modelNameLooksLikeSection(model) === section;
}

export function modelNameLooksLikeDictation(model: string): boolean {
  return DICTATION_MODEL_NAME.test(model);
}

export function modelNameLooksLikeSection(model: string): DashboardSection {
  if (DICTATION_MODEL_NAME.test(model)) return 'dictation';
  if (IMAGE_MODEL_NAME.test(model)) return 'image';
  if (MUSIC_MODEL_NAME.test(model)) return 'music';
  return 'llm';
}

export function usageForSection(
  usage: UsageRow[],
  models: ModelInfo[],
  section: DashboardSection,
): UsageRow[] {
  return usage.filter(row => usageBelongsToSection(row.model, models, section));
}

export function bucketUsageForSection(
  usage: MonthlyUsageRow[],
  models: ModelInfo[],
  section: DashboardSection,
): MonthlyUsageRow[] {
  return usage.filter(row => usageBelongsToSection(row.model, models, section));
}

export function modalityLabel(modality?: string): string {
  if (modality === 'audio_transcription') return 'Speech to text';
  if (modality === 'audio_speech') return 'Text to speech';
  if (modality === 'embedding') return 'Embeddings';
  if (modality === 'image') return 'Image';
  if (modality === 'audio_generation') return 'Music generation';
  return 'Language model';
}

export function sectionLabel(section: DashboardSection): string {
  if (section === 'dictation') return 'Dictation';
  if (section === 'image') return 'Image';
  if (section === 'music') return 'Music';
  return 'LLM';
}
