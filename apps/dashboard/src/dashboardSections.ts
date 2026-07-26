import type { ModelInfo, MonthlyUsageRow, UsageRow } from './types';

export type DashboardSection = 'llm' | 'dictation';

const DICTATION_MODALITIES = new Set(['audio_transcription', 'audio_speech']);
const DICTATION_MODEL_NAME = /(?:^|[-_.])(sapi|whisper|parakeet|supertonic|speech|tts|stt|asr)(?:$|[-_.])/i;

export function isDictationModel(model: Pick<ModelInfo, 'modality'> | undefined): boolean {
  return DICTATION_MODALITIES.has(model?.modality ?? '');
}

export function modelBelongsToSection(
  model: Pick<ModelInfo, 'modality'> | undefined,
  section: DashboardSection,
): boolean {
  return section === 'dictation' ? isDictationModel(model) : !isDictationModel(model);
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
  // Historical rows can outlive current registration. Keep known speech
  // runtimes in Dictation so old SAPI/Whisper data never pollutes LLM totals.
  return info
    ? modelBelongsToSection(info, section)
    : section === 'dictation'
      ? modelNameLooksLikeDictation(model)
      : !modelNameLooksLikeDictation(model);
}

export function modelNameLooksLikeDictation(model: string): boolean {
  return DICTATION_MODEL_NAME.test(model);
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
  return 'Language model';
}

export function sectionLabel(section: DashboardSection): string {
  return section === 'dictation' ? 'Dictation' : 'LLM';
}
