export class QueueStore {
    queue = [];
    listeners = new Map();
    enqueue(item) {
        const existing = this.queue.find((q) => q.id === item.id);
        if (existing) {
            existing.priority = item.priority;
            existing.status = item.status ?? "queued";
            this.notify();
            return;
        }
        this.queue.push({
            id: item.id,
            priority: item.priority,
            createdAt: Date.now(),
            status: item.status ?? "queued",
        });
        this.notify();
    }
    dequeue() {
        const available = this.queue.filter((q) => q.status === "queued");
        if (available.length === 0)
            return undefined;
        const sorted = [...available].sort((a, b) => {
            if (b.priority !== a.priority)
                return b.priority - a.priority;
            return a.createdAt - b.createdAt;
        });
        return sorted[0];
    }
    getById(id) {
        return this.queue.find((q) => q.id === id);
    }
    updateStatus(id, status) {
        const item = this.queue.find((q) => q.id === id);
        if (!item)
            return;
        item.status = status;
        this.notify();
    }
    remove(id) {
        this.queue = this.queue.filter((q) => q.id !== id);
        this.notify();
    }
    getQueued(limit, skip) {
        return this.queue
            .filter((q) => q.status === "queued")
            .sort((a, b) => {
            if (b.priority !== a.priority)
                return b.priority - a.priority;
            return a.createdAt - b.createdAt;
        })
            .slice(skip, skip + limit);
    }
    getLeased() {
        return this.queue.filter((q) => q.status === "leased");
    }
    getPaused() {
        return this.queue.filter((q) => q.status === "paused");
    }
    getDeadLetter() {
        return this.queue.filter((q) => q.status === "dead_letter");
    }
    get all() {
        return [...this.queue];
    }
    get length() {
        return this.queue.filter((q) => q.status === "queued").length;
    }
    get gpuLocked() {
        return this.queue.some((q) => q.status === "leased");
    }
    get estimatedWaitMs() {
        return this.queue.filter((q) => q.status === "leased").length * 5000;
    }
    positionOf(jobId) {
        const item = this.queue.find((q) => q.id === jobId);
        if (!item || item.status !== "queued")
            return -1;
        return this.queue.filter((q) => {
            if (q.status !== "queued")
                return false;
            if (q.priority > item.priority)
                return true;
            return q.priority === item.priority && q.createdAt < item.createdAt;
        }).length;
    }
    clearFailed() {
        const before = this.queue.length;
        this.queue = this.queue.filter((q) => q.status !== "dead_letter");
        this.notify();
        return before - this.queue.length;
    }
    clearPaused() {
        const before = this.queue.filter((q) => q.status === "paused").length;
        this.queue = this.queue.filter((q) => q.status !== "paused");
        this.notify();
        return before;
    }
    pauseAll() {
        let count = 0;
        for (const q of this.queue) {
            if (q.status === "queued") {
                q.status = "paused";
                count++;
            }
        }
        this.notify();
        return count;
    }
    resumeAll() {
        let count = 0;
        for (const q of this.queue) {
            if (q.status === "paused") {
                q.status = "queued";
                count++;
            }
        }
        this.notify();
        return count;
    }
    staleJobCount(thresholdMs) {
        const leased = this.getLeased();
        const stale = [];
        const now = Date.now();
        for (const q of leased) {
            if (now - q.createdAt > thresholdMs) {
                stale.push({
                    jobId: q.id,
                    stuckSince: new Date(q.createdAt).toISOString(),
                    lastHeartbeat: null,
                    reason: "stale",
                });
            }
        }
        return stale;
    }
    lease(jobId) {
        const item = this.queue.find((q) => q.id === jobId);
        if (!item)
            return false;
        item.status = "leased";
        this.notify();
        return true;
    }
    unlease(jobId) {
        const item = this.queue.find((q) => q.id === jobId);
        if (!item)
            return;
        item.status = "queued";
        this.notify();
    }
    on(event, cb) {
        if (!this.listeners.has(event)) {
            this.listeners.set(event, new Set());
        }
        this.listeners.get(event).add(cb);
        return () => {
            this.listeners.get(event)?.delete(cb);
        };
    }
    notify() {
        const cbs = this.listeners.get("changed");
        if (cbs) {
            for (const cb of cbs)
                cb();
        }
    }
    subscribe(cb) {
        return this.on("changed", cb);
    }
    getSnapshot() {
        const q = this.queue;
        return {
            queued: q.filter((x) => x.status === "queued").length,
            leased: q.filter((x) => x.status === "leased").length,
            paused: q.filter((x) => x.status === "paused").length,
            total: q.length,
            gpuLocked: q.some((x) => x.status === "leased"),
            lockedBy: q.find((x) => x.status === "leased")?.id ?? null,
        };
    }
    toJSON() {
        return [...this.queue];
    }
    restoreFromJSON(items) {
        this.queue = items.map((item) => ({ ...item }));
        this.notify();
    }
}
//# sourceMappingURL=QueueStore.js.map