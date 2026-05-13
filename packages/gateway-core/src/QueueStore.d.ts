import type { StaleJobState } from "@r9700/shared/jobTypes";
export declare class QueueStore {
    private queue;
    private listeners;
    enqueue(item: {
        id: string;
        priority: number;
        status?: "queued" | "leased" | "paused" | "dead_letter";
    }): void;
    dequeue(): typeof this.queue[0] | undefined;
    getById(id: string): typeof this.queue[0] | undefined;
    updateStatus(id: string, status: "queued" | "leased" | "paused" | "dead_letter"): void;
    remove(id: string): void;
    getQueued(limit: number, skip: number): typeof this.queue[0][];
    getLeased(): typeof this.queue[0][];
    getPaused(): typeof this.queue[0][];
    getDeadLetter(): typeof this.queue[0][];
    get all(): typeof this.queue[0][];
    get length(): number;
    get gpuLocked(): boolean;
    get estimatedWaitMs(): number;
    positionOf(jobId: string): number;
    clearFailed(): number;
    clearPaused(): number;
    pauseAll(): number;
    resumeAll(): number;
    staleJobCount(thresholdMs: number): StaleJobState[];
    lease(jobId: string): boolean;
    unlease(jobId: string): void;
    on(event: string, cb: () => void): () => void;
    private notify;
    subscribe(cb: () => void): () => void;
    getSnapshot(): {
        queued: number;
        leased: number;
        paused: number;
        total: number;
        gpuLocked: boolean;
        lockedBy: string | null;
    };
    toJSON(): typeof this.queue;
    restoreFromJSON(items: typeof this.queue): void;
}
//# sourceMappingURL=QueueStore.d.ts.map