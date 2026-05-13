import type { FastifyRequest } from "fastify";
import type { AppContext } from "../app";
import { createJobRepository } from "../db/repositories/jobs.repo";

export interface WorkloadLease {
  jobId: string;
  resourceClass: string;
  release: (status: "succeeded" | "failed" | "cancelled", data?: Record<string, unknown>) => void;
}

export class WorkloadCoordinator {
  constructor(private readonly ctx: AppContext) {}

  async acquire(req: FastifyRequest, input: {
    type: string;
    resourceClass: string;
    priority: number;
    payload: Record<string, unknown>;
    requestPath: string;
    requestMethod: string;
  }): Promise<WorkloadLease> {
    const repo = createJobRepository(this.ctx.db);
    const jobId = globalThis.crypto?.randomUUID?.() ?? `job-${Date.now()}-${Math.random().toString(36).slice(2)}`;
    repo.insert({
      id: jobId,
      type: input.type,
      priority: input.priority,
      resourceClass: input.resourceClass,
      clientName: String(req.headers["x-forwarded-for"] ?? req.ip ?? "direct_api"),
      requestPath: input.requestPath,
      requestMethod: input.requestMethod,
      payload: input.payload,
      maxRetries: 0,
      idempotencyKey: String(req.headers["idempotency-key"] ?? "") || null,
    });
    repo.insertEvent(jobId, "created", "Job created from API request", { path: input.requestPath });
    this.ctx.queueStore.enqueue({ id: jobId, priority: input.priority });
    this.ctx.events.emit("job:created", { jobId, type: input.type });
    this.ctx.events.emit("queue:changed", this.ctx.queueStore.getSnapshot());
    this.ctx.logs.write({ level: "info", service: "scheduler", jobId, message: "Job queued", data: { resourceClass: input.resourceClass } });

    const timeoutMs = this.ctx.config.scheduler.maxHiddenInteractiveWaitMs;
    const started = Date.now();
    while (Date.now() - started <= timeoutMs) {
      const job = repo.getById(jobId);
      if (!job || job.status === "cancelled") throw Object.assign(new Error("Job was cancelled before execution"), { statusCode: 499 });
      const next = this.ctx.queueStore.getQueued(1, 0)[0];
      if (next?.id === jobId && this.ctx.queueStore.getLeased().length === 0) {
        this.ctx.queueStore.lease(jobId);
        repo.updateStatus(jobId, "running", { started_at: new Date().toISOString(), lease_until: new Date(Date.now() + this.ctx.config.scheduler.jobLeaseSeconds * 1000).toISOString() });
        repo.insertEvent(jobId, "running", "GPU lease acquired", { waitedMs: Date.now() - started });
        this.ctx.metrics.record("queue", "wait_ms", Date.now() - started, "ms");
        this.ctx.events.emit("job:updated", { jobId, status: "running" });
        this.ctx.events.emit("queue:changed", this.ctx.queueStore.getSnapshot());
        return {
          jobId,
          resourceClass: input.resourceClass,
          release: (status, data = {}) => this.release(jobId, status, data),
        };
      }
      await new Promise((resolve) => setTimeout(resolve, 125));
    }

    repo.updateStatus(jobId, "failed", { error_json: JSON.stringify({ error: "queue_timeout" }), finished_at: new Date().toISOString() });
    repo.insertEvent(jobId, "failed", "Timed out waiting for GPU lease", { timeoutMs });
    this.ctx.queueStore.remove(jobId);
    this.ctx.events.emit("job:updated", { jobId, status: "failed" });
    this.ctx.events.emit("queue:changed", this.ctx.queueStore.getSnapshot());
    throw Object.assign(new Error("Timed out waiting for GPU lease"), { statusCode: 429, retryAfter: this.ctx.config.scheduler.defaultRetryAfterSeconds });
  }

  private release(jobId: string, status: "succeeded" | "failed" | "cancelled", data: Record<string, unknown>): void {
    const repo = createJobRepository(this.ctx.db);
    const finished_at = new Date().toISOString();
    if (status === "succeeded") {
      repo.updateStatus(jobId, "succeeded", { result_json: JSON.stringify(data), finished_at });
    } else if (status === "cancelled") {
      repo.updateStatus(jobId, "cancelled", { finished_at });
    } else {
      repo.updateStatus(jobId, "failed", { error_json: JSON.stringify(data), finished_at });
    }
    repo.insertEvent(jobId, status, `Job ${status}`, data);
    this.ctx.queueStore.remove(jobId);
    this.ctx.events.emit(status === "cancelled" ? "job:cancelled" : "job:updated", { jobId, status });
    this.ctx.events.emit("queue:changed", this.ctx.queueStore.getSnapshot());
    this.ctx.logs.write({ level: status === "failed" ? "error" : "info", service: "scheduler", jobId, message: `Job ${status}`, data });
  }
}
