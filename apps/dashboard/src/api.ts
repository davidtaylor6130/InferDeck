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

export function swapTo(model: string): Promise<{ status: string }> {
  return postJson<{ status: string }>(`/v1/swap/to/${encodeURIComponent(model)}`);
}

export function cancelSwap(): Promise<{ status: string }> {
  return postJson<{ status: string }>('/v1/swap/cancel');
}

export function unloadModel(): Promise<{ ok: boolean }> {
  return postJson<{ ok: boolean }>('/api/models/unload');
}

export function eventStreamUrl(): string {
  return `${API_BASE}/api/events/stream`;
}
