export class Scheduler {
    config;
    mode;
    queueStore;
    onModeChange;
    onPausedJobs;
    onJobEvent;
    constructor({ config = {}, mode = "ai", queueStore, onModeChange, }) {
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
    setMode(mode) {
        const prev = this.mode;
        this.mode = mode;
        if (prev !== mode && this.onModeChange) {
            this.onModeChange(mode);
        }
    }
    getMode() {
        return this.mode;
    }
    get queueStoreRef() {
        return this.queueStore;
    }
    enqueue(job) {
        const status = "status" in job ? job.status : "queued";
        this.queueStore.enqueue({
            id: job.id,
            priority: job.priority,
            status: status,
        });
        this.notify("job:enqueued", job);
    }
    notify(event, data) {
        this.onJobEvent?.(event, data);
    }
    getNextJob() {
        const queued = this.queueStore.getQueued(1, 0);
        return queued.length > 0 ? queued[0] : null;
    }
    shouldRunImmediate(job) {
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
    pauseQueue() {
        return this.queueStore.pauseAll();
    }
    resumeQueue() {
        return this.queueStore.resumeAll();
    }
    getQueueSnapshot() {
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
//# sourceMappingURL=Scheduler.js.map