export class ResourceLockManager {
    locks = new Map();
    leases = new Map();
    config;
    gamingMode = {
        active: false,
        rejectInteractiveLlm: true,
        pauseBackgroundJobs: true,
        unloadOllamaModels: true,
        stopComfyUi: false,
    };
    onGamingModeChange;
    constructor(config) {
        this.config = {
            maxConcurrentGpuHeavyJobs: 1,
            gpuHeavyClasses: ["gpu_llm", "gpu_image", "gpu_audio"],
            mutuallyExclusiveClasses: ["gpu_llm", "gpu_image", "gpu_audio"],
            allowCpuWithGpu: true,
            ...config,
        };
    }
    get lockCount() {
        return this.locks.size;
    }
    get isLocked() {
        return this.locks.size > 0;
    }
    get activeLock() {
        const entries = Array.from(this.locks.values());
        return entries.length > 0 ? entries[0] : null;
    }
    getActiveResourceClass() {
        const lock = this.activeLock;
        return lock ? lock.resourceClass : null;
    }
    acquire(jobId, resourceClass) {
        if (!this.config.mutuallyExclusiveClasses.includes(resourceClass)) {
            this.locks.set(jobId, {
                jobId,
                resourceClass,
                acquiredAt: new Date().toISOString(),
            });
            return { locked: true, reason: "non_gpu_resource" };
        }
        if (this.locks.size > 0) {
            const existing = Array.from(this.locks.values())[0];
            const otherClass = existing?.resourceClass;
            if (otherClass &&
                this.config.mutuallyExclusiveClasses.includes(otherClass)) {
                if (otherClass === resourceClass &&
                    this.config.maxConcurrentGpuHeavyJobs >= 1) {
                    this.locks.set(jobId, {
                        jobId,
                        resourceClass,
                        acquiredAt: new Date().toISOString(),
                    });
                    return { locked: true, reason: "gpu_concurrent_allowed" };
                }
                return { locked: false, reason: `gpu_conflict: ${otherClass}` };
            }
        }
        if (this.config.maxConcurrentGpuHeavyJobs > 0 &&
            this.countGpuHeavyLocks() >= this.config.maxConcurrentGpuHeavyJobs) {
            return { locked: false, reason: "max_concurrent_reached" };
        }
        this.locks.set(jobId, {
            jobId,
            resourceClass,
            acquiredAt: new Date().toISOString(),
        });
        return { locked: true, reason: "gpu_available" };
    }
    countGpuHeavyLocks() {
        return Array.from(this.locks.values()).filter((l) => this.config.gpuHeavyClasses.includes(l.resourceClass)).length;
    }
    setGamingMode(mode) {
        const prevActive = this.gamingMode.active;
        this.gamingMode = { ...this.gamingMode, ...mode };
        if (prevActive !== this.gamingMode.active && this.onGamingModeChange) {
            this.onGamingModeChange(this.gamingMode.active);
        }
    }
    getGamingMode() {
        return { ...this.gamingMode };
    }
    isGamingModeActive() {
        return this.gamingMode.active;
    }
    shouldRejectInteractiveLlm() {
        return this.gamingMode.active && this.gamingMode.rejectInteractiveLlm;
    }
    shouldPauseBackgroundJobs() {
        return this.gamingMode.active && this.gamingMode.pauseBackgroundJobs;
    }
    shouldUnloadOllamaModels() {
        return this.gamingMode.active && this.gamingMode.unloadOllamaModels;
    }
    release(jobId) {
        if (!this.locks.has(jobId))
            return false;
        this.locks.delete(jobId);
        this.leases.delete(jobId);
        return true;
    }
    releaseAll() {
        const count = this.locks.size;
        this.locks.clear();
        this.leases.clear();
        return count;
    }
    canRunWithActive(resourceClass) {
        if (this.isLocked) {
            const active = this.activeLock;
            if (!active)
                return true;
            if (!this.config.gpuHeavyClasses.includes(active.resourceClass))
                return true;
            if (resourceClass === active.resourceClass)
                return (this.countGpuHeavyLocks() < this.config.maxConcurrentGpuHeavyJobs);
            if (this.config.allowCpuWithGpu &&
                !this.config.gpuHeavyClasses.includes(resourceClass))
                return true;
            return false;
        }
        return true;
    }
    tryLease(jobId, leaseDurationMs, lockType = "gpu") {
        const lease = {
            jobId,
            leaseUntil: new Date(Date.now() + leaseDurationMs).toISOString(),
            lastHeartbeat: new Date().toISOString(),
            lockedAt: new Date().toISOString(),
            lockType,
        };
        this.leases.set(jobId, lease);
        return { leased: true };
    }
    hasLease(jobId) {
        return this.leases.has(jobId);
    }
    getLease(jobId) {
        return this.leases.get(jobId) ?? null;
    }
    isLeaseExpired(jobId) {
        const lease = this.leases.get(jobId);
        if (!lease)
            return true;
        return Date.now() > new Date(lease.leaseUntil).getTime();
    }
    heartbeat(jobId) {
        const lease = this.leases.get(jobId);
        if (!lease)
            return false;
        lease.lastHeartbeat = new Date().toISOString();
        return true;
    }
    getAcquiredAt() {
        const lock = this.activeLock;
        return lock ? lock.acquiredAt : null;
    }
    getExpiredLease() {
        for (const [, lease] of this.leases) {
            if (Date.now() > new Date(lease.leaseUntil).getTime()) {
                this.leases.delete(lease.jobId);
                return lease;
            }
        }
        return null;
    }
    clearExpiredLeases() {
        const expired = [];
        for (const [jobId, lease] of this.leases) {
            if (Date.now() > new Date(lease.leaseUntil).getTime()) {
                this.leases.delete(jobId);
                expired.push(lease);
            }
        }
        return expired;
    }
    toJSON() {
        return {
            locks: Array.from(this.locks.values()),
            leases: Array.from(this.leases.values()),
        };
    }
}
//# sourceMappingURL=ResourceLockManager.js.map