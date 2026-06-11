import type { Tone } from './types';

export const API_BASE: string =
  (typeof import.meta !== 'undefined' && (import.meta as { env?: Record<string, string> }).env?.VITE_API_BASE) || '';

export function formatBytes(bytes?: number | null): string {
  if (!bytes || bytes <= 0) return 'N/A';
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), sizes.length - 1);
  return `${(bytes / Math.pow(1024, i)).toFixed(i < 2 ? 0 : 1)} ${sizes[i]}`;
}

export function formatMb(mb?: number | null): string {
  if (mb == null || mb < 0) return 'N/A';
  if (mb >= 1024) return `${(mb / 1024).toFixed(1)} GB`;
  return `${Math.round(mb)} MB`;
}

export function timeAgo(value?: number | Date | string | null): string {
  if (!value) return 'N/A';
  const time = value instanceof Date ? value.getTime() : typeof value === 'number' ? value : new Date(value).getTime();
  if (!Number.isFinite(time)) return 'N/A';
  const seconds = Math.max(0, Math.round((Date.now() - time) / 1000));
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

export function formatDuration(ms?: number | null): string {
  if (ms == null || ms < 0) return 'N/A';
  if (ms < 1000) return `${Math.round(ms)} ms`;
  if (ms < 60_000) return `${(ms / 1000).toFixed(1)} s`;
  return `${Math.floor(ms / 60_000)}m ${Math.round((ms % 60_000) / 1000)}s`;
}

export function formatDate(unixMs?: number | string | null): string {
  if (!unixMs) return 'N/A';
  const date = typeof unixMs === 'number' ? new Date(unixMs) : new Date(unixMs);
  if (Number.isNaN(date.getTime())) return 'N/A';
  return date.toLocaleString([], { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });
}

export function formatTokenCount(value?: number | null): string {
  const count = Number(value ?? 0);
  if (count >= 1_000_000_000) return `${(count / 1_000_000_000).toFixed(2)}B`;
  if (count >= 1_000_000) return `${(count / 1_000_000).toFixed(count >= 10_000_000 ? 0 : 1)}M`;
  if (count >= 1_000) return `${(count / 1_000).toFixed(1)}K`;
  return `${count}`;
}

export function formatCurrency(value: number): string {
  return `$${value.toLocaleString(undefined, { maximumFractionDigits: value < 10 ? 2 : 0 })}`;
}

export function compactModel(model: string): string {
  const file = model.split(/[\\/]/).pop() || model;
  return file.length > 32 ? `${file.slice(0, 29)}...` : file;
}

export function clamp(value: number, min: number, max: number): number {
  return Math.max(min, Math.min(value, max));
}

export function threshold(value?: number | null): Tone {
  if (value == null) return 'idle';
  if (value >= 95) return 'critical';
  if (value >= 80) return 'warn';
  return 'good';
}

export function temperatureTone(value?: number | null): Tone {
  if (value == null || value <= 0) return 'idle';
  if (value >= 90) return 'critical';
  if (value >= 78) return 'warn';
  return 'good';
}

export function toneText(tone: Tone): string {
  if (tone === 'good') return 'text-success-green';
  if (tone === 'warn') return 'text-warning-amber';
  if (tone === 'critical') return 'text-danger-rose';
  if (tone === 'info') return 'text-queue-blue';
  if (tone === 'violet') return 'text-infer-violet';
  return 'text-text-secondary';
}

export function toneBg(tone: Tone): string {
  if (tone === 'good') return 'bg-success-green/10';
  if (tone === 'warn') return 'bg-warning-amber/10';
  if (tone === 'critical') return 'bg-danger-rose/10';
  if (tone === 'info') return 'bg-queue-blue/10';
  if (tone === 'violet') return 'bg-infer-violet/10';
  return 'bg-white/[0.04]';
}

export function toneHex(tone: Tone): string {
  if (tone === 'good') return '#22C55E';
  if (tone === 'warn') return '#F59E0B';
  if (tone === 'critical') return '#F43F5E';
  if (tone === 'info') return '#60A5FA';
  if (tone === 'violet') return '#8B5CF6';
  return '#64748B';
}
