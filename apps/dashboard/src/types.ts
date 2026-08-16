export type Tone = 'good' | 'warn' | 'critical' | 'idle' | 'info' | 'violet';

export type ConnectionState = 'connecting' | 'connected' | 'reconnecting' | 'offline';

export interface ModelOptimizationInfo {
  status: string;
  measured_at?: string;
  quality_passes?: number;
  quality_total?: number;
  single_tokens_per_second?: number;
  parallel_tokens_per_second?: number;
  schedule_enabled?: boolean;
  schedule_window_start?: string;
  schedule_window_end?: string;
}

export interface ModelInfo {
  id: string;
  family?: string;
  runtime?: string;
  runtime_available?: boolean;
  modality?: string;
  capabilities?: string[];
  context_size: number;
  vram_required_mb: number;
  n_slots: number;
  has_vision: boolean;
  loaded: boolean;
  primary?: boolean;
  free_slots?: number;
  active_requests?: number;
  optimization?: ModelOptimizationInfo;
  prompt_price_per_million?: number;
  completion_price_per_million?: number;
  alias?: boolean;
  alias_target?: string;
  required_context_size?: number;
  required_capabilities?: string[];
}

export interface GpuSample {
  available: boolean;
  name: string;
  utilizationPct: number;
  vramUsedMb: number;
  vramTotalMb?: number;
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
  state: 'swapping' | 'waiting' | 'ready' | 'failed' | 'cancelled' | 'unloaded';
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
  generationDurationMs?: number;
  tokensPerSecond: number;
  promptTokensPerSecond?: number;
  status: number;
  inputAudioSeconds?: number;
  inputCharacters?: number;
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
  cachedPromptTokens?: number;
  completionTokens: number;
  measuredCompletionTokens?: number;
  measuredPromptTokens?: number;
  totalTokens: number;
  peakTokensPerSecond: number;
  avgTokensPerSecond: number;
  peakPromptTokensPerSecond?: number;
  avgPromptTokensPerSecond?: number;
  lastTimestampUnixMs: number;
  inputAudioSeconds?: number;
  inputCharacters?: number;
}

export interface MonthlyUsageRow {
  bucket: string;
  model: string;
  promptTokens: number;
  cachedPromptTokens?: number;
  completionTokens: number;
  measuredCompletionTokens?: number;
  measuredPromptTokens?: number;
  totalTokens: number;
  requests: number;
  successfulRequests: number;
  generationDurationMs?: number;
  promptDurationMs?: number;
  peakTokensPerSecond?: number;
  peakPromptTokensPerSecond?: number;
  inputAudioSeconds?: number;
  inputCharacters?: number;
}

export interface StatusPayload {
  status: string;
  queue: {
    running: number;
    queued?: number;
    gpuLocked: boolean;
    lockOwner: string;
    vramBudgetMb?: number;
    vramAvailableMb?: number;
    resourceDecision?: string;
    requests?: Array<{ id: number; model: string; priority: number; position: number; queuedMs: number; remainingMs: number }>;
  };
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
  dailyTokenUsageAllTime?: boolean;
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
  resolvedModel?: string;
  createdAt: string;
  timestampUnixMs: number;
  promptTokens: number;
  cachedPromptTokens?: number;
  completionTokens: number;
  totalTokens: number;
  tokensPerSecond: number;
  promptTokensPerSecond?: number;
  generationDurationMs?: number;
  promptDurationMs?: number;
  durationMs: number;
  httpStatus: number;
  slotId: number;
  inputAudioSeconds?: number;
  inputCharacters?: number;
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
  billing_unit?: 'tokens' | 'audio_minute' | 'million_characters';
  price_per_unit?: number;
  source_url?: string;
  source?: 'model_settings' | string;
}

export interface HealthPayload {
  ok: boolean;
  db_healthy: boolean;
  db_path?: string;
  gpu_available?: boolean;
  gpu_provider?: string;
  requests?: number;
}

export interface ActivityItem {
  id: string;
  kind: 'request' | 'swap';
  label: string;
  detail: string;
  timestampUnixMs: number;
  tone: Tone;
}
