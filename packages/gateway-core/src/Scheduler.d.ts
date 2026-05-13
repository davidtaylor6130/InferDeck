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
export declare class Scheduler {
    private config;
    mode: GatewayMode;
    queueStore: QueueStore;
    onModeChange?: (mode: GatewayMode) => void;
    onPausedJobs?: (jobIds: string[]) => void;
    onJobEvent?: (event: string, data: unknown) => void;
    constructor({ config, mode, queueStore, onModeChange, }: SchedulerOptions);
    setMode(mode: GatewayMode): void;
    getMode(): GatewayMode;
    get queueStoreRef(): QueueStore;
    enqueue(job: JobRecord | JobContext): void;
    private notify;
    getNextJob(): JobRecord | null;
    shouldRunImmediate(job: {
        resourceClass: string;
        priority: number;
    }): {
        shouldRun: boolean;
        reason: string;
        retryAfter?: number;
    };
    pauseQueue(): number;
    resumeQueue(): number;
    getQueueSnapshot(): {
        queueLength: number;
        running: number;
        paused: number;
        estimatedWaitMs: number;
        gpuLocked: boolean;
        gpuLockedBy: string | null;
    };
}
export {};
//# sourceMappingURL=Scheduler.d.ts.map