import type { ResourceClass } from "@r9700/shared/apiTypes";
import type { Lease } from "@r9700/shared/jobTypes";

interface ResourceLockState {
  jobId: string;
  resourceClass: ResourceClass;
  acquiredAt: string;
}

interface ResourceConfig {
  maxConcurrentGpuHeavyJobs: number;
  gpuHeavyClasses: ResourceClass[];
  mutuallyExclusiveClasses: ResourceClass[];
  allowCpuWithGpu: boolean;
}

interface GamingModeState {
  active: boolean;
  rejectInteractiveLlm: boolean;
  pauseBackgroundJobs: boolean;
  unloadBackendModels: boolean;
  stopComfyUi: boolean;
}

export class ResourceLockManager {
  private locks: Map<string, ResourceLockState> = new Map();
  private leases: Map<string, Lease> = new Map();
  private config: ResourceConfig;
  private gamingMode: GamingModeState = {
    active: false,
    rejectInteractiveLlm: true,
    pauseBackgroundJobs: true,
    unloadBackendModels: true,
    stopComfyUi: false,
  };
  onGamingModeChange?: (active: boolean) => void;

  constructor(config?: Partial<ResourceConfig>) {
    this.config = {
      maxConcurrentGpuHeavyJobs: 1,
      gpuHeavyClasses: ["gpu_llm", "gpu_image", "gpu_audio"],
      mutuallyExclusiveClasses: ["gpu_llm", "gpu_image", "gpu_audio"],
      allowCpuWithGpu: true,
      ...config,
    };
  }

  get lockCount(): number {
    return this.locks.size;
  }

  get isLocked(): boolean {
    return this.locks.size > 0;
  }

  get activeLock(): ResourceLockState | null {
    const entries = Array.from(this.locks.values());
    return entries.length > 0 ? entries[0] : null;
  }

  getActiveResourceClass(): ResourceClass | null {
    const lock = this.activeLock;
    return lock ? lock.resourceClass : null;
  }

  acquire(
    jobId: string,
    resourceClass: ResourceClass
  ): { locked: boolean; reason: string } {
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
      if (
        otherClass &&
        this.config.mutuallyExclusiveClasses.includes(otherClass)
      ) {
        if (
          otherClass === resourceClass &&
          this.config.maxConcurrentGpuHeavyJobs >= 1
        ) {
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

    if (
      this.config.maxConcurrentGpuHeavyJobs > 0 &&
      this.countGpuHeavyLocks() >= this.config.maxConcurrentGpuHeavyJobs
    ) {
      return { locked: false, reason: "max_concurrent_reached" };
    }

    this.locks.set(jobId, {
      jobId,
      resourceClass,
      acquiredAt: new Date().toISOString(),
    });
    return { locked: true, reason: "gpu_available" };
  }

  countGpuHeavyLocks(): number {
    return Array.from(this.locks.values()).filter((l) =>
      this.config.gpuHeavyClasses.includes(l.resourceClass)
    ).length;
  }

  setGamingMode(mode: GamingModeState): void {
    const prevActive = this.gamingMode.active;
    this.gamingMode = { ...this.gamingMode, ...mode };
    if (prevActive !== this.gamingMode.active && this.onGamingModeChange) {
      this.onGamingModeChange(this.gamingMode.active);
    }
  }

  getGamingMode(): GamingModeState {
    return { ...this.gamingMode };
  }

  isGamingModeActive(): boolean {
    return this.gamingMode.active;
  }

  shouldRejectInteractiveLlm(): boolean {
    return this.gamingMode.active && this.gamingMode.rejectInteractiveLlm;
  }

  shouldPauseBackgroundJobs(): boolean {
    return this.gamingMode.active && this.gamingMode.pauseBackgroundJobs;
  }

  shouldUnloadBackendModels(): boolean {
    return this.gamingMode.active && this.gamingMode.unloadBackendModels;
  }

  release(jobId: string): boolean {
    if (!this.locks.has(jobId)) return false;
    this.locks.delete(jobId);
    this.leases.delete(jobId);
    return true;
  }

  releaseAll(): number {
    const count = this.locks.size;
    this.locks.clear();
    this.leases.clear();
    return count;
  }

  canRunWithActive(resourceClass: ResourceClass): boolean {
    if (this.isLocked) {
      const active = this.activeLock;
      if (!active) return true;
      if (!this.config.gpuHeavyClasses.includes(active.resourceClass))
        return true;
      if (resourceClass === active.resourceClass)
        return (
          this.countGpuHeavyLocks() < this.config.maxConcurrentGpuHeavyJobs
        );
      if (
        this.config.allowCpuWithGpu &&
        !this.config.gpuHeavyClasses.includes(resourceClass)
      )
        return true;
      return false;
    }
    return true;
  }

  tryLease(
    jobId: string,
    leaseDurationMs: number,
    lockType: Lease["lockType"] = "gpu"
  ): { leased: boolean } {
    const lease: Lease = {
      jobId,
      leaseUntil: new Date(Date.now() + leaseDurationMs).toISOString(),
      lastHeartbeat: new Date().toISOString(),
      lockedAt: new Date().toISOString(),
      lockType,
    };
    this.leases.set(jobId, lease);
    return { leased: true };
  }

  hasLease(jobId: string): boolean {
    return this.leases.has(jobId);
  }

  getLease(jobId: string): Lease | null {
    return this.leases.get(jobId) ?? null;
  }

  isLeaseExpired(jobId: string): boolean {
    const lease = this.leases.get(jobId);
    if (!lease) return true;
    return Date.now() > new Date(lease.leaseUntil).getTime();
  }

  heartbeat(jobId: string): boolean {
    const lease = this.leases.get(jobId);
    if (!lease) return false;
    lease.lastHeartbeat = new Date().toISOString();
    return true;
  }

  getAcquiredAt(): string | null {
    const lock = this.activeLock;
    return lock ? lock.acquiredAt : null;
  }

  getExpiredLease(): Lease | null {
    for (const [, lease] of this.leases) {
      if (Date.now() > new Date(lease.leaseUntil).getTime()) {
        this.leases.delete(lease.jobId);
        return lease;
      }
    }
    return null;
  }

  clearExpiredLeases(): Lease[] {
    const expired: Lease[] = [];
    for (const [jobId, lease] of this.leases) {
      if (Date.now() > new Date(lease.leaseUntil).getTime()) {
        this.leases.delete(jobId);
        expired.push(lease);
      }
    }
    return expired;
  }

  toJSON(): { locks: ResourceLockState[]; leases: Lease[] } {
    return {
      locks: Array.from(this.locks.values()),
      leases: Array.from(this.leases.values()),
    };
  }
}
