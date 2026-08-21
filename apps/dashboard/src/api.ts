import type {
  JobRecord,
  HealthPayload,
  ModelInfo,
  MonthlyUsageRow,
  PricingEntry,
  StatusPayload,
  SwapHistoryRow,
} from './types';
import { API_BASE } from './utils';

const CONTROL_API_BASE = '/api/inferdeck/v1';

async function getJson<T>(path: string, timeoutMs = 15_000): Promise<T> {
  const response = await fetch(`${API_BASE}${path}`, {
    signal: AbortSignal.timeout(timeoutMs),
    headers: { Accept: 'application/json' },
  });
  if (!response.ok) throw new Error(`${path} responded ${response.status}`);
  return (await response.json()) as T;
}

async function postJson<T>(path: string, body?: unknown, timeoutMs = 30_000): Promise<T> {
  const response = await fetch(`${API_BASE}${path}`, {
    method: 'POST',
    signal: AbortSignal.timeout(timeoutMs),
    headers: { Accept: 'application/json', 'Content-Type': 'application/json' },
    body: body == null ? undefined : JSON.stringify(body),
  });
  const payload = (await response.json().catch(() => ({}))) as T & { error?: { message?: string } };
  if (!response.ok && response.status !== 202) {
    throw new Error(payload?.error?.message || `${path} responded ${response.status}`);
  }
  return payload;
}

async function putJson<T>(path: string, body: unknown, timeoutMs = 30_000): Promise<T> {
  const response = await fetch(`${API_BASE}${path}`, {
    method: 'PUT',
    signal: AbortSignal.timeout(timeoutMs),
    headers: { Accept: 'application/json', 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  const payload = (await response.json().catch(() => ({}))) as T & { error?: { message?: string } };
  if (!response.ok) throw new Error(payload?.error?.message || `${path} responded ${response.status}`);
  return payload;
}

async function deleteJson<T>(path: string, timeoutMs = 30_000, headers?: Record<string, string>): Promise<T> {
  const response = await fetch(`${API_BASE}${path}`, {
    method: 'DELETE',
    signal: AbortSignal.timeout(timeoutMs),
    headers: { Accept: 'application/json', 'Content-Type': 'application/json', ...headers },
  });
  const payload = (await response.json().catch(() => ({}))) as T & { error?: { message?: string } };
  if (!response.ok) throw new Error(payload?.error?.message || `${path} responded ${response.status}`);
  return payload;
}

export interface ConfigDocument {
  yaml: string;
  revision: string;
  activeYaml: string;
  activeRevision: string;
  runningRevision: string;
  hasActiveProfile: boolean;
  usingActiveProfile: boolean;
  fallbackReason?: string;
  restartRequired: boolean;
  secretSentinel?: string;
}

export interface ConfigApplyResult {
  ok: boolean;
  activeRevision: string;
  hasActiveProfile: boolean;
  restartRequired: boolean;
  applyScheduled: boolean;
}

export interface ProfileOptimizationInput {
  model: string;
  contextPerSlot: number;
  slots: number;
  minSlots: number;
  nBatch: number;
  nUbatch: number;
  cacheTypeK: string;
  cacheTypeV: string;
  flashAttention?: string;
}

export interface ProfileOptimizationCandidate {
  contextPerSlot: number;
  slots: number;
  nBatch: number;
  nUbatch: number;
  cacheTypeK: string;
  cacheTypeV: string;
  flashAttention: string;
  mtpMaxActiveRequests: number;
  estimatedVramMb: number;
  reserveVramMb: number;
  qualityScore: number;
  speedScore: number;
  parallelismScore: number;
  headroomScore: number;
  overallScore: number;
  fits: boolean;
  reasons: string[];
}

export interface ProfileOptimizationResult {
  model: string;
  mode: 'profile_estimate';
  measured: false;
  observedTokensPerSecond: number;
  modelFileMb: number;
  totalVramMb: number;
  weights: {
    quality: number;
    speed: number;
    parallelism: number;
    headroom: number;
  };
  recommended: ProfileOptimizationCandidate;
  candidates: ProfileOptimizationCandidate[];
  notes: string[];
}

export interface ProfileBenchmarkCandidate extends ProfileOptimizationCandidate {
  completed: boolean;
  error: string;
  loadMs: number;
  promptTokensPerSecond: number;
  averageTokensPerSecond: number;
  parallelTokensPerSecond: number;
  averageTimeToFirstTokenMs: number;
  peakVramMb: number;
  qualityPasses: number;
  qualityTotal: number;
  performanceIndex: number;
  promptTokens: number;
  completionTokens: number;
  concurrency: Array<{
    requests: number;
    aggregateTokensPerSecond: number;
    averageRequestTokensPerSecond: number;
    mtpRequests: number;
    mtpDraftedTokens: number;
    mtpAcceptedTokens: number;
  }>;
  outputSamples: string[];
}

export interface ProfileBenchmarkSnapshot {
  id: number;
  state: 'idle' | 'running' | 'cancelling' | 'completed' | 'cancelled' | 'failed';
  stage: string;
  message: string;
  model: string;
  completedCandidates: number;
  totalCandidates: number;
  progressPct: number;
  startedUnixMs: number;
  finishedUnixMs: number;
  measured: true;
  cancelRequested: boolean;
  restored: boolean;
  weights: {
    promptProcessing: number;
    generation: number;
  };
  baseline: ProfileBenchmarkCandidate | null;
  recommended: ProfileOptimizationCandidate | null;
  candidates: ProfileBenchmarkCandidate[];
}

export interface StoreModel {
  id: string;
  pipeline: string;
  runtime: string;
  modality: string;
  downloads: number;
  likes: number;
  private: boolean;
  gated: boolean | string;
  lastModified?: string;
  hasVision?: boolean;
  recommended?: boolean;
}

export interface StoreFile {
  repo: string;
  revision: string;
  name: string;
  size: number;
  sha256: string;
  runtime: string;
  modality: string;
  capabilities: string[];
  format: string;
  quantization: string;
  compatible: boolean;
  estimatedRamMb: number;
  estimatedVramMb: number;
  artifactCount?: number;
}

export interface StoreDownload {
  id: number;
  repo: string;
  filename: string;
  modelName: string;
  runtime: string;
  modality: string;
  state: string;
  error: string;
  bytesDownloaded: number;
  bytesTotal: number;
  installedPath: string;
  artifactCount?: number;
}

export interface InstalledStoreModel {
  id?: string;
  name?: string;
  family?: string;
  runtime?: string;
  modality?: string;
  capabilities?: string[];
  path?: string;
  size?: number;
  sha256?: string;
  vramRequiredMb?: number;
  quantization?: string;
  artifactCount?: number;
  hasVision?: boolean;
  configured?: boolean;
  managed?: boolean;
}

export interface MediaJob {
  id: number;
  model: string;
  modality: string;
  progress: number;
  state: string;
}

type JsonObject = Record<string, unknown>;

function asObject(value: unknown): JsonObject {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as JsonObject
    : {};
}

function asBoolean(value: unknown): boolean | undefined {
  return typeof value === 'boolean' ? value : undefined;
}

function asString(value: unknown): string | undefined {
  return typeof value === 'string' ? value : undefined;
}

function asNumber(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) && value >= 0 ? value : undefined;
}

function asStringArray(value: unknown): string[] | undefined {
  return Array.isArray(value) && value.every(item => typeof item === 'string')
    ? value
    : undefined;
}

function normalizeModel(value: unknown): ModelInfo | null {
  const entry = asObject(value);
  const id = asString(entry.id);
  if (!id) return null;

  const inferdeck = asObject(entry.inferdeck);
  const resources = asObject(inferdeck.resources);
  const residency = asObject(inferdeck.residency);
  const optimization = asObject(
    Object.keys(asObject(entry.optimization)).length
      ? entry.optimization
      : inferdeck.optimization,
  );
  const loaded = asBoolean(residency.loaded) ?? asBoolean(entry.loaded) ?? false;
  const configuredSlots = asNumber(entry.n_slots) ?? asNumber(resources.configured_slots) ?? 0;
  const actualSlots = asNumber(residency.slots);
  const normalized: ModelInfo = {
    id,
    family: asString(entry.family),
    runtime: asString(entry.runtime) ?? asString(inferdeck.runtime),
    runtime_available: asBoolean(entry.runtime_available) ?? asBoolean(inferdeck.runtime_available),
    modality: asString(entry.modality) ?? asString(inferdeck.modality),
    capabilities: asStringArray(entry.capabilities) ?? asStringArray(inferdeck.capabilities),
    context_size: asNumber(entry.context_size) ?? 0,
    vram_required_mb: asNumber(entry.vram_required_mb) ?? asNumber(resources.vram_required_mb) ?? 0,
    n_slots: loaded && actualSlots !== undefined ? actualSlots : configuredSlots,
    has_vision: asBoolean(entry.has_vision) ?? false,
    loaded,
    primary: asBoolean(residency.primary) ?? asBoolean(entry.primary),
    free_slots: asNumber(residency.free_slots) ?? asNumber(entry.free_slots),
    active_requests: asNumber(residency.active_requests) ?? asNumber(entry.active_requests),
    prompt_price_per_million: asNumber(entry.prompt_price_per_million),
    cached_prompt_price_per_million: asNumber(entry.cached_prompt_price_per_million),
    completion_price_per_million: asNumber(entry.completion_price_per_million),
    alias: asBoolean(entry.alias),
    alias_target: asString(entry.alias_target),
    required_context_size: asNumber(entry.required_context_size),
    required_capabilities: asStringArray(entry.required_capabilities),
  };
  const optimizationStatus = asString(optimization.status);
  if (optimizationStatus) {
    normalized.optimization = {
      status: optimizationStatus,
      measured_at: asString(optimization.measured_at),
      quality_passes: asNumber(optimization.quality_passes),
      quality_total: asNumber(optimization.quality_total),
      single_tokens_per_second: asNumber(optimization.single_tokens_per_second),
      parallel_tokens_per_second: asNumber(optimization.parallel_tokens_per_second),
      schedule_enabled: asBoolean(optimization.schedule_enabled),
      schedule_window_start: asString(optimization.schedule_window_start),
      schedule_window_end: asString(optimization.schedule_window_end),
    };
  }
  const resizing = asBoolean(residency.resizing) ?? asBoolean(entry.resizing);
  if (resizing !== undefined) {
    (normalized as ModelInfo & { resizing?: boolean }).resizing = resizing;
  }
  return normalized;
}

export function getStatus(): Promise<StatusPayload> {
  return getJson<StatusPayload>(`${CONTROL_API_BASE}/status`);
}

export function getDailyUsage(): Promise<{
  dailyTokenUsage: MonthlyUsageRow[];
  dailyTokenUsageAllTime: boolean;
}> {
  return getJson(`${CONTROL_API_BASE}/usage/daily`);
}

export async function getModels(): Promise<ModelInfo[]> {
  const body = await getJson<{ models?: unknown }>(`${CONTROL_API_BASE}/models`);
  if (!Array.isArray(body.models)) return [];
  return body.models
    .map(normalizeModel)
    .filter((model): model is ModelInfo => model !== null);
}

export async function getJobs(limit = 100): Promise<JobRecord[]> {
  const body = await getJson<{ jobs: JobRecord[] }>(`${CONTROL_API_BASE}/jobs?limit=${limit}`);
  return Array.isArray(body.jobs) ? body.jobs : [];
}

export async function getSwapHistory(): Promise<SwapHistoryRow[]> {
  const body = await getJson<{ swaps: SwapHistoryRow[] }>(`${CONTROL_API_BASE}/stats/history`);
  return Array.isArray(body.swaps) ? body.swaps : [];
}

export async function getLogs(limit = 250): Promise<string[]> {
  const body = await getJson<{ logs: Array<{ message: string }> }>(`${CONTROL_API_BASE}/logs?limit=${limit}`);
  return Array.isArray(body.logs) ? body.logs.map(line => line.message) : [];
}

export async function getPricing(): Promise<PricingEntry[]> {
  const body = await getJson<PricingEntry[]>(`${CONTROL_API_BASE}/pricing`);
  return Array.isArray(body) ? body : [];
}

export function getHealth(): Promise<HealthPayload> {
  return getJson<HealthPayload>(`${CONTROL_API_BASE}/health`);
}

export interface ModelAliasRecord {
  name: string;
  target: string;
  requiredContextSize: number;
  requiredCapabilities: string[];
}

export interface ModelAliasDocument {
  aliases: ModelAliasRecord[];
  revision: string;
}

export async function getModelAliases(): Promise<ModelAliasDocument> {
  const body = await getJson<{ aliases?: ModelAliasRecord[]; revision?: string }>(`${CONTROL_API_BASE}/model-aliases`);
  return { aliases: Array.isArray(body.aliases) ? body.aliases : [], revision: body.revision ?? '' };
}

export function saveModelAlias(name: string, target: string, revision: string): Promise<ModelAliasRecord & { revision: string }> {
  return putJson<ModelAliasRecord & { revision: string }>(`${CONTROL_API_BASE}/model-aliases/${encodeURIComponent(name)}`, { target, revision });
}

export function deleteModelAlias(name: string, revision: string): Promise<{ ok: boolean; revision: string }> {
  return deleteJson(`${CONTROL_API_BASE}/model-aliases/${encodeURIComponent(name)}`, 30_000, { 'If-Match': revision });
}

export function getConfig(): Promise<ConfigDocument> {
  return getJson<ConfigDocument>(`${CONTROL_API_BASE}/config`);
}

export function saveConfig(yaml: string, revision: string): Promise<ConfigDocument & { ok: boolean }> {
  return putJson<ConfigDocument & { ok: boolean }>(`${CONTROL_API_BASE}/config`, { yaml, revision });
}

export function saveActiveConfig(yaml: string, revision: string): Promise<ConfigApplyResult> {
  return putJson<ConfigApplyResult>(`${CONTROL_API_BASE}/config/active`, { yaml, revision });
}

export function optimizeProfile(input: ProfileOptimizationInput): Promise<ProfileOptimizationResult> {
  return postJson<ProfileOptimizationResult>(`${CONTROL_API_BASE}/optimize/profile`, input);
}

export function startProfileBenchmark(
  input: ProfileOptimizationInput & { candidateLimit?: number },
): Promise<ProfileBenchmarkSnapshot> {
  return postJson<ProfileBenchmarkSnapshot>(`${CONTROL_API_BASE}/optimize/benchmark`, input);
}

export function getProfileBenchmark(): Promise<ProfileBenchmarkSnapshot> {
  return getJson<ProfileBenchmarkSnapshot>(`${CONTROL_API_BASE}/optimize/benchmark`);
}

export function cancelProfileBenchmark(): Promise<ProfileBenchmarkSnapshot> {
  return postJson<ProfileBenchmarkSnapshot>(`${CONTROL_API_BASE}/optimize/benchmark/cancel`, {});
}

export interface ScheduledOptimizationRecord {
  model: string;
  enabled: boolean;
  windowStart: string;
  windowEnd: string;
  nextRunUnixMs: number;
  lastStartedUnixMs: number;
  lastFinishedUnixMs: number;
  lastOutcome: string;
  lastMessage: string;
}

export function getOptimizationSchedule(): Promise<{ timezone: string; schedules: ScheduledOptimizationRecord[] }> {
  return getJson(`${CONTROL_API_BASE}/optimize/schedule`);
}

export function resetActiveConfig(revision: string): Promise<{ ok: boolean; removed: boolean; restartRequired: boolean; applyScheduled: boolean }> {
  return deleteJson(`${CONTROL_API_BASE}/config/active`, 30_000, { 'If-Match': revision });
}

const delay = (milliseconds: number) =>
  new Promise<void>(resolve => globalThis.setTimeout(resolve, milliseconds));

class ConfigFallbackError extends Error {}

async function waitForConfig(
  predicate: (config: ConfigDocument) => boolean,
  description: string,
  timeoutMs: number,
  pollMs: number,
): Promise<ConfigDocument> {
  const deadline = Date.now() + timeoutMs;
  let lastFailure = 'InferDeck has not returned yet.';
  while (Date.now() < deadline) {
    try {
      const config = await getJson<ConfigDocument>(`${CONTROL_API_BASE}/config`, Math.min(3_000, timeoutMs));
      if (config.fallbackReason) {
        throw new ConfigFallbackError(`InferDeck rejected the saved profile: ${config.fallbackReason}`);
      }
      if (predicate(config)) return config;
      lastFailure = 'InferDeck returned, but the requested profile is not active yet.';
    } catch (error) {
      if (error instanceof ConfigFallbackError) throw error;
      lastFailure = error instanceof Error ? error.message : String(error);
    }
    await delay(pollMs);
  }
  throw new Error(`Timed out waiting for ${description}. ${lastFailure}`);
}

export function waitForActiveConfig(
  revision: string,
  timeoutMs = 180_000,
  pollMs = 750,
): Promise<ConfigDocument> {
  return waitForConfig(
    config => config.usingActiveProfile &&
      config.activeRevision === revision &&
      config.runningRevision === revision,
    'InferDeck to apply the active profile',
    timeoutMs,
    pollMs,
  );
}

export function waitForStableConfig(
  timeoutMs = 180_000,
  pollMs = 750,
): Promise<ConfigDocument> {
  return waitForConfig(
    config => !config.hasActiveProfile &&
      !config.usingActiveProfile &&
      config.runningRevision === config.revision,
    'InferDeck to restore the stable profile',
    timeoutMs,
    pollMs,
  );
}

export async function searchStore(query: string, runtime = '', modality = '', limit = 50): Promise<StoreModel[]> {
  const params = new URLSearchParams({ q: query, limit: String(limit) });
  if (runtime) params.set('runtime', runtime);
  if (modality) params.set('modality', modality);
  const body = await getJson<{ models: StoreModel[] }>(`${CONTROL_API_BASE}/model-store/search?${params}`);
  return body.models;
}

export async function inspectStoreModel(repo: string): Promise<StoreFile[]> {
  const body = await getJson<{ files: StoreFile[] }>(`${CONTROL_API_BASE}/model-store/inspect?repo=${encodeURIComponent(repo)}`);
  return body.files;
}

export function installStoreModel(file: StoreFile, modelName: string): Promise<{ id: number; state: string }> {
  return postJson<{ id: number; state: string }>(`${CONTROL_API_BASE}/model-store/downloads`, {
    repo: file.repo, filename: file.name, runtime: file.runtime, modality: file.modality, modelName,
  });
}

export async function getStoreActivity(): Promise<{
  downloads: StoreDownload[];
  installed: Record<string, InstalledStoreModel>;
  library: InstalledStoreModel[];
}> {
  return getJson(`${CONTROL_API_BASE}/model-store/downloads`);
}

export function controlStoreDownload(id: number, action: 'cancel' | 'resume'): Promise<{ ok: boolean }> {
  return postJson<{ ok: boolean }>(`${CONTROL_API_BASE}/model-store/downloads/${id}/${action}`);
}

export function removeStoreModel(model: string, action: 'archive' | 'remove'): Promise<{ ok: boolean }> {
  return postJson<{ ok: boolean }>(`${CONTROL_API_BASE}/model-store/${action}`, { model });
}

export async function unregisterConfiguredModel(model: string): Promise<{
  ok: boolean;
  filesDeleted: boolean;
  restartRequired: boolean;
}> {
  const config = await getConfig();
  return postJson(`${CONTROL_API_BASE}/model-store/unregister`, {
    model,
    revision: config.activeRevision,
  });
}

export async function getMediaJobs(): Promise<MediaJob[]> {
  const body = await getJson<{ jobs: MediaJob[] }>(`${CONTROL_API_BASE}/media/jobs`);
  return body.jobs;
}

export function cancelMediaJob(id: number): Promise<{ ok: boolean }> {
  return postJson<{ ok: boolean }>(`${CONTROL_API_BASE}/media/jobs/${id}/cancel`);
}

export function swapTo(model: string): Promise<{ status: string }> {
  return postJson<{ status: string }>(`${CONTROL_API_BASE}/swap/to/${encodeURIComponent(model)}`);
}

export function cancelSwap(): Promise<{ status: string }> {
  return postJson<{ status: string }>(`${CONTROL_API_BASE}/swap/cancel`);
}

export function unloadModel(model?: string): Promise<{ ok: boolean }> {
  return postJson<{ ok: boolean }>(`${CONTROL_API_BASE}/models/unload`, model ? { model } : undefined);
}

export function eventStreamUrl(): string {
  return `${API_BASE}${CONTROL_API_BASE}/events/stream`;
}
