import type { StaleJobState } from "@r9700/shared/jobTypes";

export class QueueStore {
  private queue: Array<{
    id: string;
    priority: number;
    createdAt: number;
    status: "queued" | "leased" | "paused" | "dead_letter";
  }> = [];
  private listeners = new Map<string, Set<() => void>>();

  enqueue(item: {
    id: string;
    priority: number;
    status?: "queued" | "leased" | "paused" | "dead_letter";
  }): void {
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

  dequeue(): typeof this.queue[0] | undefined {
    const available = this.queue.filter((q) => q.status === "queued");
    if (available.length === 0) return undefined;
    const sorted = [...available].sort((a, b) => {
      if (b.priority !== a.priority) return b.priority - a.priority;
      return a.createdAt - b.createdAt;
    });
    return sorted[0];
  }

  getById(id: string): typeof this.queue[0] | undefined {
    return this.queue.find((q) => q.id === id);
  }

  updateStatus(id: string, status: "queued" | "leased" | "paused" | "dead_letter"): void {
    const item = this.queue.find((q) => q.id === id);
    if (!item) return;
    item.status = status;
    this.notify();
  }

  remove(id: string): void {
    this.queue = this.queue.filter((q) => q.id !== id);
    this.notify();
  }

  getQueued(limit: number, skip: number): typeof this.queue[0][] {
    return this.queue
      .filter((q) => q.status === "queued")
      .sort((a, b) => {
        if (b.priority !== a.priority) return b.priority - a.priority;
        return a.createdAt - b.createdAt;
      })
      .slice(skip, skip + limit);
  }

  getLeased(): typeof this.queue[0][] {
    return this.queue.filter((q) => q.status === "leased");
  }

  getPaused(): typeof this.queue[0][] {
    return this.queue.filter((q) => q.status === "paused");
  }

  getDeadLetter(): typeof this.queue[0][] {
    return this.queue.filter((q) => q.status === "dead_letter");
  }

  get all(): typeof this.queue[0][] {
    return [...this.queue];
  }

  get length(): number {
    return this.queue.filter((q) => q.status === "queued").length;
  }

  get gpuLocked(): boolean {
    return this.queue.some((q) => q.status === "leased");
  }

  get estimatedWaitMs(): number {
    return this.queue.filter((q) => q.status === "leased").length * 5000;
  }

  positionOf(jobId: string): number {
    const item = this.queue.find((q) => q.id === jobId);
    if (!item || item.status !== "queued") return -1;
    return this.queue.filter((q) => {
      if (q.status !== "queued") return false;
      if (q.priority > item.priority) return true;
      return q.priority === item.priority && q.createdAt < item.createdAt;
    }).length;
  }

  clearFailed(): number {
    const before = this.queue.length;
    this.queue = this.queue.filter(
      (q) => q.status !== "dead_letter"
    );
    this.notify();
    return before - this.queue.length;
  }

  clearPaused(): number {
    const before = this.queue.filter((q) => q.status === "paused").length;
    this.queue = this.queue.filter((q) => q.status !== "paused");
    this.notify();
    return before;
  }

  pauseAll(): number {
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

  resumeAll(): number {
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

  staleJobCount(thresholdMs: number): StaleJobState[] {
    const leased = this.getLeased();
    const stale: StaleJobState[] = [];
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

  lease(jobId: string): boolean {
    const item = this.queue.find((q) => q.id === jobId);
    if (!item) return false;
    item.status = "leased";
    this.notify();
    return true;
  }

  unlease(jobId: string): void {
    const item = this.queue.find((q) => q.id === jobId);
    if (!item) return;
    item.status = "queued";
    this.notify();
  }

  on(event: string, cb: () => void): () => void {
    if (!this.listeners.has(event)) {
      this.listeners.set(event, new Set());
    }
    this.listeners.get(event)!.add(cb);
    return () => {
      this.listeners.get(event)?.delete(cb);
    };
  }

  private notify(): void {
    const cbs = this.listeners.get("changed");
    if (cbs) {
      for (const cb of cbs) cb();
    }
  }

  subscribe(cb: () => void): () => void {
    return this.on("changed", cb);
  }

  getSnapshot(): {
    queued: number;
    leased: number;
    paused: number;
    total: number;
    gpuLocked: boolean;
    lockedBy: string | null;
  } {
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

  toJSON(): typeof this.queue {
    return [...this.queue];
  }

  restoreFromJSON(items: typeof this.queue): void {
    this.queue = items.map((item) => ({ ...item }));
    this.notify();
  }
}
