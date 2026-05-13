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
    unloadOllamaModels: boolean;
    stopComfyUi: boolean;
}
export declare class ResourceLockManager {
    private locks;
    private leases;
    private config;
    private gamingMode;
    onGamingModeChange?: (active: boolean) => void;
    constructor(config?: Partial<ResourceConfig>);
    get lockCount(): number;
    get isLocked(): boolean;
    get activeLock(): ResourceLockState | null;
    getActiveResourceClass(): ResourceClass | null;
    acquire(jobId: string, resourceClass: ResourceClass): {
        locked: boolean;
        reason: string;
    };
    countGpuHeavyLocks(): number;
    setGamingMode(mode: GamingModeState): void;
    getGamingMode(): GamingModeState;
    isGamingModeActive(): boolean;
    shouldRejectInteractiveLlm(): boolean;
    shouldPauseBackgroundJobs(): boolean;
    shouldUnloadOllamaModels(): boolean;
    release(jobId: string): boolean;
    releaseAll(): number;
    canRunWithActive(resourceClass: ResourceClass): boolean;
    tryLease(jobId: string, leaseDurationMs: number, lockType?: Lease["lockType"]): {
        leased: boolean;
    };
    hasLease(jobId: string): boolean;
    getLease(jobId: string): Lease | null;
    isLeaseExpired(jobId: string): boolean;
    heartbeat(jobId: string): boolean;
    getAcquiredAt(): string | null;
    getExpiredLease(): Lease | null;
    clearExpiredLeases(): Lease[];
    toJSON(): {
        locks: ResourceLockState[];
        leases: Lease[];
    };
}
export {};
//# sourceMappingURL=ResourceLockManager.d.ts.map