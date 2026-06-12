export type Tone = 'good' | 'warn' | 'critical' | 'idle' | 'info' | 'violet';

export type ConnectionState = 'connecting' | 'connected' | 'reconnecting' | 'offline';

export interface ModelInfo {
  id: string;
  family?: string;
  context_size: number;
  vram_required_mb: number;
  n_slots: number;
  has_vision: boolean;
  loaded: boolean;
}

export interface GpuSample {
  available: boolean;
  name: string;
  utilizationPct: number;
  vramUsedMb: number;
  temperatureC: number;
  powerW: number;
}

export interface StatsEvent {
  timestampUnixMs: number;
  gpu: GpuSample;
  loadedModel: string;
  activeRequests: number;
  swapping: boolean;
  swapTarget: string;
  totalRequests: number;
  totalSwaps: number;
  lifetimeTokensIn: number;
  lifetimeTokensOut: number;
  avgTokensPerSecond: number;
  uptimeSeconds: number;
}

export interface ModelEvent {
  state: 'swapping' | 'ready' | 'failed' | 'cancelled' | 'unloaded';
  from: string;
  to: string;
  durationMs: number;
  error: string;
  timestampUnixMs: number;
}

export interface RequestEvent {
  timestampUnixMs: number;
  model: string;
  promptTokens: number;
  completionTokens: number;
  durationMs: number;
  tokensPerSecond: number;
  status: number;
}

export interface SwapState {
  swapping: boolean;
  target: string;
  from: string;
  startedUnixMs: number;
  lastError: string;
}

export interface UsageRow {
  model: string;
  requests: number;
  successfulRequests: number;
  promptTokens: number;
  completionTokens: number;
  totalTokens: number;
  peakTokensPerSecond: number;
  avgTokensPerSecond: number;
  lastTimestampUnixMs: number;
}

export interface MonthlyUsageRow {
  bucket: string;
  model: string;
  promptTokens: number;
  completionTokens: number;
  totalTokens: number;
  requests: number;
  successfulRequests: number;
}

export interface StatusPayload {
  status: string;
  queue: { running: number; gpuLocked: boolean; lockOwner: string };
  swap: SwapState;
  hardware: {
    available?: boolean;
    provider?: string;
    gpu?: Record<string, unknown>;
    memory?: { used: number; total: number; percentage: number };
    cpu?: { name: string; logicalProcessors: number };
  };
  summary: {
    totalRequests: number;
    totalTokens: number;
    promptTokens: number;
    completionTokens: number;
    avgLatencyMs: number;
    p50LatencyMs: number;
    p95LatencyMs: number;
  };
  metrics: {
    total_requests: number;
    total_swaps: number;
    total_tokens: number;
    avg_tokens_per_second: number;
  };
  tokenUsage: UsageRow[];
  monthlyTokenUsage: MonthlyUsageRow[];
  dailyTokenUsage?: MonthlyUsageRow[];
  hourlyTokenUsage?: MonthlyUsageRow[];
  models: Array<Omit<ModelInfo, 'id'> & { id: string }>;
  current: string;
  uptime: number;
}

export interface JobRecord {
  id: string;
  type: string;
  status: 'succeeded' | 'failed';
  model: string;
  createdAt: string;
  timestampUnixMs: number;
  promptTokens: number;
  completionTokens: number;
  totalTokens: number;
  tokensPerSecond: number;
  durationMs: number;
  httpStatus: number;
  slotId: number;
}

export interface SwapHistoryRow {
  timestamp_unix_ms: number;
  from_model: string;
  to_model: string;
  duration_ms: number;
  success: boolean;
  error: string;
}

export interface PricingEntry {
  model_name: string;
  prompt_price_per_million: number;
  completion_price_per_million: number;
  equivalent_api_model?: string | null;
  currency?: string;
}

export interface ActivityItem {
  id: string;
  kind: 'request' | 'swap';
  label: string;
  detail: string;
  timestampUnixMs: number;
  tone: Tone;
}
