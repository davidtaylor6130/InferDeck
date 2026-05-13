import type {
  GatewayMode,
  ResourceClass,
} from "@r9700/shared/apiTypes";
import { ResourceLockManager } from "./ResourceLockManager.js";
import { QueueStore } from "./QueueStore.js";

interface ShouldRunResult {
  shouldRun: boolean;
  reason: string;
  retryAfter?: number;
}

interface GamingModeState {
  active: boolean;
  rejectInteractiveLlm: boolean;
  pauseBackgroundJobs: boolean;
  unloadOllamaModels: boolean;
  stopComfyUi: boolean;
}

export interface PolicyEngineOptions {
  config: {
    maxConcurrentGpuHeavyJobs: number;
    maxHiddenInteractiveWaitMs: number;
    defaultRetryAfterSeconds: number;
    staleRunningJobAfterMs: number;
    heartbeatIntervalMs: number;
    jobLeaseSeconds: number;
  };
  mode: GatewayMode;
  queueStore: QueueStore;
  lockManager: ResourceLockManager;
  onModeChange?: (mode: GatewayMode) => void;
}

export class PolicyEngine {
  private config: PolicyEngineOptions["config"];
  private _mode: GatewayMode;
  private queueStore: QueueStore;
  private lockManager: ResourceLockManager;

  constructor(options: PolicyEngineOptions) {
    this.config = options.config;
    this._mode = options.mode;
    this.queueStore = options.queueStore;
    this.lockManager = options.lockManager;

    this.lockManager.onGamingModeChange = (active: boolean) => {
      if (active && this._mode !== "gaming") {
        this._mode = "gaming";
        options.onModeChange?.("gaming");
      } else if (!active && this._mode === "gaming") {
        this._mode = "ai";
        options.onModeChange?.("ai");
      }
    };
  }

  get mode(): GatewayMode {
    return this._mode;
  }

  set mode(value: GatewayMode) {
    if (value === "gaming") {
      this.lockManager.setGamingMode({
        active: true,
        rejectInteractiveLlm: true,
        pauseBackgroundJobs: true,
        unloadOllamaModels: true,
        stopComfyUi: true,
      });
    } else {
      this.lockManager.setGamingMode({
        active: false,
        rejectInteractiveLlm: false,
        pauseBackgroundJobs: false,
        unloadOllamaModels: false,
        stopComfyUi: false,
      });
    }
    this._mode = value;
  }

  shouldRunJob(job: { resourceClass: string; priority: number }): ShouldRunResult {
    if (this._mode === "maintenance") {
      return {
        shouldRun: false,
        reason: "maintenance_mode",
        retryAfter: 0,
      };
    }

    const gpuHeavyClasses = ["gpu_llm", "gpu_image", "gpu_audio"];
    const isGpuHeavy = gpuHeavyClasses.includes(job.resourceClass);

    if (this._mode === "gaming") {
      return this.handleGamingMode(job, isGpuHeavy);
    }

    return this.handleAiMode(job, isGpuHeavy);
  }

  private handleGamingMode(
    job: { resourceClass: string; priority: number },
    _isGpuHeavy: boolean
  ): ShouldRunResult {
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

    return {
      shouldRun: false,
      reason: "gaming_mode_reject_background",
      retryAfter: this.config.defaultRetryAfterSeconds,
    };
  }

  private handleAiMode(
    job: { resourceClass: string; priority: number },
    isGpuHeavy: boolean
  ): ShouldRunResult {
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

  canExecuteGpuJob(job: { resourceClass: string }): { locked: boolean; reason: string } {
    if (this._mode === "gaming" && this.lockManager.isGamingModeActive()) {
      return { locked: false, reason: "gaming_mode_active" };
    }

    const gpuHeavyClasses = ["gpu_llm", "gpu_image", "gpu_audio"];
    if (!gpuHeavyClasses.includes(job.resourceClass)) {
      return { locked: true, reason: "non_gpu_job" };
    }

    return this.lockManager.acquire(
      globalThis.crypto.randomUUID(),
      job.resourceClass as ResourceClass
    );
  }

  handleStaleJobs(): string[] {
    const staleThreshold = this.config.staleRunningJobAfterMs;
    const leased = this.queueStore.getLeased();
    const stale: string[] = [];

    for (const job of leased) {
      if (Date.now() - job.createdAt > staleThreshold) {
        stale.push(job.id);
      }
    }

    return stale;
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

  toObject(): {
    mode: GatewayMode;
    queueSnapshot: ReturnType<PolicyEngine["getQueueSnapshot"]>;
    lockCount: number;
    gamingMode: GamingModeState;
  } {
    return {
      mode: this._mode,
      queueSnapshot: this.getQueueSnapshot(),
      lockCount: this.lockManager.lockCount,
      gamingMode: this.lockManager.getGamingMode(),
    };
  }
}
