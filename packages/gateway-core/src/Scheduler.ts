import type { GatewayMode } from "@r9700/shared/apiTypes";
import type { JobContext } from "@r9700/shared/jobTypes";
import { QueueStore } from "./QueueStore.js";

interface JobRecord {
  id: string;
  priority: number;
  status: "queued" | "leased" | "paused" | "dead_letter";
  createdAt: number;
}

export interface SchedulerOptions {
  config: {
    maxConcurrentGpuHeavyJobs?: number;
    maxHiddenInteractiveWaitMs?: number;
    defaultRetryAfterSeconds?: number;
    staleRunningJobAfterMs?: number;
    heartbeatIntervalMs?: number;
    jobLeaseSeconds?: number;
  };
  mode: GatewayMode;
  queueStore: QueueStore;
  onModeChange?: (mode: GatewayMode) => void;
}

export class Scheduler {
  private config: {
    maxConcurrentGpuHeavyJobs: number;
    maxHiddenInteractiveWaitMs: number;
    defaultRetryAfterSeconds: number;
    staleRunningJobAfterMs: number;
    heartbeatIntervalMs: number;
    jobLeaseSeconds: number;
  };
  mode: GatewayMode;
  queueStore: QueueStore;
  onModeChange?: (mode: GatewayMode) => void;
  onPausedJobs?: (jobIds: string[]) => void;
  onJobEvent?: (event: string, data: unknown) => void;

  constructor({
    config = {},
    mode = "ai",
    queueStore,
    onModeChange,
  }: SchedulerOptions) {
    this.config = {
      maxConcurrentGpuHeavyJobs: 1,
      maxHiddenInteractiveWaitMs: 20000,
      defaultRetryAfterSeconds: 30,
      staleRunningJobAfterMs: 600000,
      heartbeatIntervalMs: 5000,
      jobLeaseSeconds: 120,
      ...config,
    };
    this.mode = mode;
    this.queueStore = queueStore;
    this.onModeChange = onModeChange;
  }

  setMode(mode: GatewayMode): void {
    const prev = this.mode;
    this.mode = mode;
    if (prev !== mode && this.onModeChange) {
      this.onModeChange(mode);
    }
  }

  getMode(): GatewayMode {
    return this.mode;
  }

  get queueStoreRef(): QueueStore {
    return this.queueStore;
  }

  enqueue(job: JobRecord | JobContext): void {
    const status = "status" in job ? (job.status as "queued" | "leased" | "paused") : "queued";
    this.queueStore.enqueue({
      id: job.id,
      priority: job.priority,
      status: status,
    });
    this.notify("job:enqueued", job);
  }

  private notify(event: string, data: unknown): void {
    this.onJobEvent?.(event, data);
  }

  getNextJob(): JobRecord | null {
    const queued = this.queueStore.getQueued(1, 0);
    return queued.length > 0 ? queued[0] : null;
  }

  shouldRunImmediate(job: { resourceClass: string; priority: number }): {
    shouldRun: boolean;
    reason: string;
    retryAfter?: number;
  } {
    if (this.mode === "maintenance") {
      return { shouldRun: false, reason: "maintenance_mode" };
    }

    const gpuHeavyClasses = ["gpu_llm", "gpu_image", "gpu_audio"];
    const isGpuHeavy = gpuHeavyClasses.includes(job.resourceClass);

    if (this.mode === "gaming" && isGpuHeavy) {
      const isInteractive = job.priority >= 60;
      if (isInteractive) {
        const running = this.queueStore.getLeased().length;
        if (running === 0) {
          return { shouldRun: true, reason: "gaming_mode_gpu_free" };
        }
        return {
          shouldRun: false,
          reason: "interactive_rejected_in_gaming_mode",
          retryAfter: this.config.defaultRetryAfterSeconds,
        };
      }
      return { shouldRun: false, reason: "gaming_mode_reject_background" };
    }

    if (isGpuHeavy && this.queueStore.getLeased().length > 0) {
      return {
        shouldRun: false,
        reason: "gpu_busy",
        retryAfter: this.config.defaultRetryAfterSeconds,
      };
    }

    if (isGpuHeavy) {
      return { shouldRun: true, reason: "gpu_available" };
    }

    const isInteractive = job.priority >= 60;
    if (isInteractive) {
      const estimatedWait = this.queueStore.getLeased().length * 5000;
      if (estimatedWait <= this.config.maxHiddenInteractiveWaitMs) {
        return { shouldRun: true, reason: "hold_and_run" };
      }
      return {
        shouldRun: false,
        reason: "interactive_wait_too_long",
        retryAfter: this.config.defaultRetryAfterSeconds,
      };
    }

    return { shouldRun: true, reason: "non_gpu_no_lock" };
  }

  pauseQueue(): number {
    return this.queueStore.pauseAll();
  }

  resumeQueue(): number {
    return this.queueStore.resumeAll();
  }

  getQueueSnapshot(): {
    queueLength: number;
    running: number;
    paused: number;
    estimatedWaitMs: number;
    gpuLocked: boolean;
    gpuLockedBy: string | null;
  } {
    const leased = this.queueStore.getLeased();
    return {
      queueLength: this.queueStore.length,
      running: leased.length,
      paused: this.queueStore.getPaused().length,
      estimatedWaitMs: leased.length * 5000,
      gpuLocked: leased.length > 0,
      gpuLockedBy: leased.length > 0 ? leased[0].id : null,
    };
  }
}
