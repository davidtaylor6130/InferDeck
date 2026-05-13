/**
 * Job runtime types internal to the scheduler.
 */

export type ClientOrigin =
  | "open_webui"
  | "opencode"
  | "n8n"
  | "direct_api"
  | "dashboard"
  | "script";

export function inferClientOrigin(headers: Record<string, string>): ClientOrigin {
  const userAgent = headers["user-agent"] ?? headers["User-Agent"] ?? "";

  if (userAgent.includes("Open WebUI") || userAgent.includes("open-webui"))
    return "open_webui";
  if (userAgent.includes("opencode")) return "opencode";
  if (userAgent.includes("n8n")) return "n8n";
  if (userAgent.includes("ai-homelab-dashboard") || userAgent.includes("r9700"))
    return "dashboard";
  if (headers["x-api-key"] ?? headers["X-API-Key"]) return "direct_api";

  return "script";
}

export interface JobContext {
  id: string;
  type: string;
  status:
    | "queued"
    | "leased"
    | "running"
    | "paused"
    | "succeeded"
    | "failed"
    | "cancelled"
    | "dead_letter";
  priority: number;
  resourceClass: string;
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

export interface JobAcquireResult {
  job: JobContext;
  timeout: number;
  reason:
    | "immediate"
    | "queue"
    | "busy"
    | "gaming_mode_reject"
    | "maintenance_mode";
}

export interface ResourceLock {
  jobId: string;
  resourceClass: string;
  acquiredAt: string;
}

export interface Lease {
  jobId: string;
  leaseUntil: string;
  lastHeartbeat: string;
  lockedAt: string;
  lockType: "gpu" | "cpu" | "disk" | "network";
}

export interface SchedulerDecision {
  type: "run" | "queue" | "reject" | "pause";
  reason: string;
  retryAfterSeconds?: number;
  estimatedWaitMs?: number;
}

export interface GamingModeState {
  active: boolean;
  enabledAt: string | null;
  unloadedModels: string[];
  pausedJobs: string[];
}

export interface StaleJobState {
  jobId: string;
  stuckSince: string;
  lastHeartbeat: string | null;
  reason: string;
}

export interface DeadLetterEntry {
  jobId: string;
  reason: string;
  error: Record<string, unknown>;
  attempts: number;
  createdAt: string;
}
