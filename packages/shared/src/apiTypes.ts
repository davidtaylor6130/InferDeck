/**
 * API type definitions for the r9700-ai-gateway.
 * Shared between backend and dashboard frontend.
 */

// ─── Gateway Modes ───────────────────────────────────────────────

export type GatewayMode = "ai" | "gaming" | "maintenance";

export interface ModeState {
  mode: GatewayMode;
  enabledAt: string;
  details: Record<string, unknown>;
}

// ─── Health & Status ─────────────────────────────────────────────

export interface HealthResponse {
  status: "healthy" | "degraded" | "unhealthy";
  uptime: number;
  version: string;
  timestamp: string;
  services: Record<string, string>;
}

export interface StatusResponse {
  health: HealthResponse;
  mode: ModeState;
  queue: QueueStatus;
  modeConfig: {
    mode: GatewayMode;
    rejectInteractiveLlm: boolean;
    pauseBackgroundJobs: boolean;
    unloadOllamaModels: boolean;
    stopComfyUi: boolean;
  };
  uptimeMs: number;
}

export interface QueueStatus {
  totalQueued: number;
  totalRunning: number;
  totalLeased: number;
  totalPaused: number;
  totalDeadLetter: number;
  totalFailed: number;
  gpuLocked: boolean;
  lockedBy: string | null;
  estimatedWaitMs: number;
}

// ─── Job Types ───────────────────────────────────────────────────

export type JobType =
  | "llm_chat"
  | "llm_generate"
  | "embedding"
  | "rag_index"
  | "image_generate"
  | "transcription"
  | "voice_tts";

export type JobStatus =
  | "queued"
  | "leased"
  | "running"
  | "paused"
  | "succeeded"
  | "failed"
  | "cancelled"
  | "dead_letter";

export type ResourceClass =
  | "gpu_llm"
  | "gpu_image"
  | "gpu_audio"
  | "cpu_index"
  | "disk_heavy"
  | "network"
  | "none";

export interface JobRecord {
  id: string;
  type: JobType;
  status: JobStatus;
  priority: number;
  resourceClass: ResourceClass;
  clientName: string | null;
  requestPath: string | null;
  requestMethod: string | null;
  payload: Record<string, unknown>;
  result: Record<string, unknown> | null;
  error: Record<string, unknown> | null;
  createdAt: string;
  updatedAt: string;
  startedAt: string | null;
  finishedAt: string | null;
  leaseUntil: string | null;
  retryCount: number;
  maxRetries: number;
  idempotencyKey: string | null;
}

export interface JobEvent {
  id: number;
  jobId: string;
  eventType: string;
  message: string | null;
  data: Record<string, unknown> | null;
  createdAt: string;
}

export interface JobListResponse {
  jobs: JobRecord[];
  total: number;
  page: number;
  pageSize: number;
}

export interface JobCreateRequest {
  type: JobType;
  payload: Record<string, unknown>;
  priority?: number;
  resourceClass?: ResourceClass;
  idempotencyKey?: string;
  clientName?: string;
}

export interface JobCreateResponse {
  jobId: string;
  status: "queued";
  position: number;
}

export interface JobUpdateResponse {
  success: boolean;
  job: JobRecord;
  message: string;
}

export interface JobPositionResponse {
  jobId: string;
  position: number;
  estimatedWaitMs: number;
}

// ─── Model Types ─────────────────────────────────────────────────

export interface ModelInfo {
  name: string;
  model: string;
  size: number;
  digest: string;
  details: {
    parent_model: string;
    format: string;
    family: string;
    families: string[];
    parameter_size: string;
    quantization_level: string;
  };
}

export interface RunningModelInfo {
  name: string;
  model: string;
  size: number;
  digest: string;
  details: ModelInfo["details"];
  size_vram: number;
  expires_at: string;
  ttl: number;
}

export interface PullModelRequest {
  name: string;
  stream?: boolean;
}

export interface LoadModelRequest {
  model: string;
  keep_alive?: string;
}

export interface UnloadModelRequest {
  model: string;
}

export interface DeleteModelRequest {
  name: string;
}

export interface ListModelsResponse {
  models: ModelInfo[];
}

export interface ListRunningModelsResponse {
  running: RunningModelInfo[];
}

// ─── Service Types ───────────────────────────────────────────────

export type ServiceKind =
  | "gateway"
  | "ollama"
  | "comfyui"
  | "rag"
  | "speech";

export interface ServiceRecord {
  id: string;
  name: string;
  kind: ServiceKind;
  status: "unknown" | "starting" | "ready" | "running" | "stopped" | "error" | "unhealthy";
  pid: number | null;
  baseUrl: string | null;
  lastHealthcheckAt: string | null;
  lastError: string | null;
  updatedAt: string;
}

// ─── Config Types ────────────────────────────────────────────────

export interface ServerConfig {
  dashboardHost: string;
  dashboardPort: number;
  proxyHost: string;
  proxyPort: number;
  publicBaseUrl: string;
}

export interface SecurityConfig {
  requireApiKey: boolean;
  apiKeyEnv: string;
  allowedLanCidrs: string[];
}

export interface DatabaseConfig {
  path: string;
  walMode: boolean;
}

export interface OllamaConfig {
  enabled: boolean;
  baseUrl: string;
  manageProcess: boolean;
  executable: string;
  healthcheckIntervalMs: number;
  restartOnFailure: boolean;
}

export interface SchedulerConfig {
  maxConcurrentGpuHeavyJobs: number;
  maxHiddenInteractiveWaitMs: number;
  defaultRetryAfterSeconds: number;
  staleRunningJobAfterMs: number;
  heartbeatIntervalMs: number;
  jobLeaseSeconds: number;
}

export interface ModesConfig {
  startupMode: GatewayMode;
  gamingMode: {
    rejectInteractiveLlm: boolean;
    pauseBackgroundJobs: boolean;
    unloadOllamaModels: boolean;
    stopComfyUi: boolean;
  };
}

export interface HardwareConfig {
  provider: "null" | "amd_adlx" | "nvidia_nvml" | "nvidia_smi" | string;
  pollIntervalMs: number;
}

export interface LoggingConfig {
  level: string;
  dir: string;
  retentionDays: number;
}

export interface GatewayConfig {
  server: ServerConfig;
  security: SecurityConfig;
  database: DatabaseConfig;
  ollama: OllamaConfig;
  scheduler: SchedulerConfig;
  modes: ModesConfig;
  hardware: HardwareConfig;
  logging: LoggingConfig;
}

// ─── Hardware Metrics ────────────────────────────────────────────

export interface HardwareMetrics {
  gpu: {
    utilization: number | null;
    memoryUsed: number | null;
    memoryTotal: number | null;
    temperature: number | null;
    powerDraw: number | null;
    clockSpeed: number | null;
  };
  cpu: {
    utilization: number | null;
    loadAvg: [number, number, number];
  };
  memory: {
    used: number | null;
    total: number | null;
    percentage: number | null;
  };
  disk: {
    used: number | null;
    total: number | null;
    free: number | null;
    percentage: number | null;
  };
  timestamp: string;
}

// ─── SSE Event Types ─────────────────────────────────────────────

export type SseEventType =
  | "job:created"
  | "job:updated"
  | "job:cancelled"
  | "job:dead_letter"
  | "queue:changed"
  | "mode:changed"
  | "model:loaded"
  | "model:unloaded"
  | "service:health"
  | "service:stated"
  | "hardware:update"
  | "system:error";

export interface SseMessage {
  type: SseEventType;
  data: Record<string, unknown>;
  timestamp: string;
}

// ─── Error Response ──────────────────────────────────────────────

export interface ErrorResponse {
  error: {
    type: string;
    message: string;
    details?: Record<string, unknown>;
  };
  retryAfter?: number;
}

// ─── Proxy Request Types ─────────────────────────────────────────

export interface OllamaChatRequest {
  model: string;
  messages?: unknown[];
  stream?: boolean;
  options?: Record<string, unknown>;
  keep_alive?: string | number | null;
}

export interface OllamaGenerateRequest {
  model: string;
  prompt: string;
  stream?: boolean;
  options?: Record<string, unknown>;
  keep_alive?: string | number | null;
}

export interface OllamaEmbedRequest {
  model: string;
  input: string | string[];
  truncate?: boolean;
  keep_alive?: string | number | null;
}

export interface OpenAiChatRequest {
  model: string;
  messages: unknown[];
  stream?: boolean;
  max_tokens?: number;
  temperature?: number;
  top_p?: number;
  frequency_penalty?: number;
  presence_penalty?: number;
  stop?: string | string[];
}

// ─── Metrics Types ───────────────────────────────────────────────

export interface MetricsSample {
  source: string;
  metricName: string;
  metricValue: number;
  unit: string;
  createdAt: string;
}

export interface MetricsResponse {
  samples: MetricsSample[];
  summary: MetricsSummary;
}

export interface MetricsSummary {
  queueLengthHistory: number[];
  gpuUtilizationAvg: number | null;
  requestsLastHour: number;
  errorsLastHour: number;
  avgJobDuration: number | null;
}
