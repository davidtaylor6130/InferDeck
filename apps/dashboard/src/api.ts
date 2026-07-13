import type {
  JobRecord,
  ModelInfo,
  PricingEntry,
  StatusPayload,
  SwapHistoryRow,
} from './types';
import { API_BASE } from './utils';

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

export interface ConfigDocument {
  yaml: string;
  revision: string;
  restartRequired: boolean;
  secretSentinel?: string;
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
}

export interface MediaJob {
  id: number;
  model: string;
  modality: string;
  progress: number;
  state: string;
}

export function getStatus(): Promise<StatusPayload> {
  return getJson<StatusPayload>('/api/status');
}

export async function getModels(): Promise<ModelInfo[]> {
  const body = await getJson<{ data: ModelInfo[] }>('/v1/models');
  return Array.isArray(body.data) ? body.data : [];
}

export async function getJobs(limit = 100): Promise<JobRecord[]> {
  const body = await getJson<{ jobs: JobRecord[] }>(`/api/jobs?limit=${limit}`);
  return Array.isArray(body.jobs) ? body.jobs : [];
}

export async function getSwapHistory(): Promise<SwapHistoryRow[]> {
  const body = await getJson<{ swaps: SwapHistoryRow[] }>('/v1/stats/history');
  return Array.isArray(body.swaps) ? body.swaps : [];
}

export async function getLogs(limit = 250): Promise<string[]> {
  const body = await getJson<{ logs: Array<{ message: string }> }>(`/api/logs?limit=${limit}`);
  return Array.isArray(body.logs) ? body.logs.map(line => line.message) : [];
}

export async function getPricing(): Promise<PricingEntry[]> {
  const body = await getJson<PricingEntry[]>('/api/pricing');
  return Array.isArray(body) ? body : [];
}

export function getConfig(): Promise<ConfigDocument> {
  return getJson<ConfigDocument>('/api/config');
}

export function saveConfig(yaml: string, revision: string): Promise<ConfigDocument & { ok: boolean }> {
  return putJson<ConfigDocument & { ok: boolean }>('/api/config', { yaml, revision });
}

export async function searchStore(query: string, runtime = '', modality = ''): Promise<StoreModel[]> {
  const params = new URLSearchParams({ q: query });
  if (runtime) params.set('runtime', runtime);
  if (modality) params.set('modality', modality);
  const body = await getJson<{ models: StoreModel[] }>(`/api/model-store/search?${params}`);
  return body.models;
}

export async function inspectStoreModel(repo: string): Promise<StoreFile[]> {
  const body = await getJson<{ files: StoreFile[] }>(`/api/model-store/inspect?repo=${encodeURIComponent(repo)}`);
  return body.files;
}

export function installStoreModel(file: StoreFile, modelName: string): Promise<{ id: number; state: string }> {
  return postJson<{ id: number; state: string }>('/api/model-store/downloads', {
    repo: file.repo, filename: file.name, runtime: file.runtime, modality: file.modality, modelName,
  });
}

export async function getStoreActivity(): Promise<{ downloads: StoreDownload[]; installed: Record<string, unknown> }> {
  return getJson('/api/model-store/downloads');
}

export function controlStoreDownload(id: number, action: 'cancel' | 'resume'): Promise<{ ok: boolean }> {
  return postJson<{ ok: boolean }>(`/api/model-store/downloads/${id}/${action}`);
}

export function removeStoreModel(model: string): Promise<{ ok: boolean }> {
  return postJson<{ ok: boolean }>('/api/model-store/remove', { model });
}

export async function getMediaJobs(): Promise<MediaJob[]> {
  const body = await getJson<{ jobs: MediaJob[] }>('/api/media/jobs');
  return body.jobs;
}

export function cancelMediaJob(id: number): Promise<{ ok: boolean }> {
  return postJson<{ ok: boolean }>(`/api/media/jobs/${id}/cancel`);
}

export function swapTo(model: string): Promise<{ status: string }> {
  return postJson<{ status: string }>(`/v1/swap/to/${encodeURIComponent(model)}`);
}

export function cancelSwap(): Promise<{ status: string }> {
  return postJson<{ status: string }>('/v1/swap/cancel');
}

export function unloadModel(model?: string): Promise<{ ok: boolean }> {
  return postJson<{ ok: boolean }>('/api/models/unload', model ? { model } : undefined);
}

export function eventStreamUrl(): string {
  return `${API_BASE}/api/events/stream`;
}
