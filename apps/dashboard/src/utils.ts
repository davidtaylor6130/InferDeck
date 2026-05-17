import type { JobRecord, ServiceRecord } from './types';

export const DASHBOARD_URL = 'http://ai.homelab.com:8721';
export const GATEWAY_API = 'http://ai.homelab.com:11434';
export const OPENAI_API = 'http://ai.homelab.com:11434/v1';
export const LLAMA_BACKEND = 'http://127.0.0.1:11434';

export function formatBytes(bytes?: number | null): string {
  if (!bytes || bytes <= 0) return 'N/A';
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), sizes.length - 1);
  return `${(bytes / Math.pow(1024, i)).toFixed(i < 2 ? 0 : 1)} ${sizes[i]}`;
}

export function formatDate(dateStr?: string | null): string {
  if (!dateStr) return 'N/A';
  const date = new Date(dateStr);
  if (Number.isNaN(date.getTime())) return 'N/A';
  return date.toLocaleString([], { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });
}

export function timeAgo(date?: Date | string | null): string {
  if (!date) return 'N/A';
  const value = typeof date === 'string' ? new Date(date) : date;
  const seconds = Math.max(0, Math.round((Date.now() - value.getTime()) / 1000));
  if (seconds < 60) return `${seconds}s ago`;
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) return `${minutes}m ago`;
  const hours = Math.round(minutes / 60);
  if (hours < 48) return `${hours}h ago`;
  return `${Math.round(hours / 24)}d ago`;
}

export function formatUptime(seconds?: number | null): string {
  if (!seconds || seconds < 0) return 'N/A';
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  return days > 0 ? `${days}d ${hours}h ${minutes}m` : `${hours}h ${minutes}m`;
}

export function modeLabel(mode?: string): string {
  if (mode === 'gaming') return 'Gaming';
  if (mode === 'maintenance') return 'Maintenance';
  return 'AI Mode';
}

export function isOnlineStatus(status?: string): boolean {
  return status === 'running' || status === 'ready' || status === 'healthy';
}

export function getQueueCounts(statusData: Record<string, any> | null, jobs: JobRecord[]) {
  const queue = statusData?.queue ?? {};
  return {
    queued: Number(queue.totalQueued ?? queue.queued ?? jobs.filter(j => j.status === 'queued').length ?? 0),
    running: Number(queue.totalRunning ?? queue.running ?? jobs.filter(j => j.status === 'running' || j.status === 'leased').length ?? 0),
    paused: Number(queue.totalPaused ?? queue.paused ?? jobs.filter(j => j.status === 'paused').length ?? 0),
    failed: Number(queue.totalFailed ?? queue.failed ?? jobs.filter(j => j.status === 'failed' || j.status === 'dead_letter').length ?? 0),
    gpuLocked: Boolean(queue.gpuLocked ?? queue.gpu_locked ?? jobs.some(j => j.status === 'running' || j.status === 'leased')),
    lockOwner: queue.lockOwner ?? queue.lock_owner ?? null,
  };
}

export function getServiceName(service: ServiceRecord): string {
  if (service.name) return service.name;
  if (service.kind === 'llama_cpp') return 'llama.cpp';
  if (service.kind === 'gateway') return 'Gateway';
  return service.kind.replace(/[-_]/g, ' ').replace(/\b\w/g, m => m.toUpperCase());
}

export function stringifyPreview(value: unknown): string {
  if (value == null) return 'No data';
  if (typeof value === 'string') return value;
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return String(value);
  }
}

export function formatError(value: unknown): string {
  if (value == null) return 'Unknown error';
  if (value instanceof Error) return value.message;
  if (typeof value === 'string') return value;
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}
