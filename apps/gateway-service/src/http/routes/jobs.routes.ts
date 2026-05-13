import type { FastifyInstance } from "fastify";
import { createJobRepository } from "../../db/repositories/jobs.repo";

function parseJson(value: unknown): unknown {
  if (typeof value !== "string" || !value) return null;
  try { return JSON.parse(value); } catch { return value; }
}

function mapJob(row: any): any {
  if (!row) return row;
  return {
    id: row.id,
    type: row.type,
    status: row.status,
    priority: row.priority,
    resourceClass: row.resource_class,
    resource_class: row.resource_class,
    clientName: row.client_name,
    client: row.client_name,
    requestPath: row.request_path,
    requestMethod: row.request_method,
    payload: parseJson(row.payload_json),
    result: parseJson(row.result_json),
    error: parseJson(row.error_json),
    createdAt: row.created_at,
    created_at: row.created_at,
    updatedAt: row.updated_at,
    startedAt: row.started_at,
    started_at: row.started_at,
    finishedAt: row.finished_at,
    completedAt: row.finished_at,
    leaseUntil: row.lease_until,
    retryCount: row.retry_count,
    maxRetries: row.max_retries,
    idempotencyKey: row.idempotency_key,
  };
}

export function registerJobsRoutes(app: FastifyInstance): void {
  app.get("/jobs", async (req, reply) => {
    const ctx = (app as any).ctx();
    const repo = createJobRepository(ctx.db);
    const query = req.query as { status?: string; limit?: string; offset?: string };
    const limit = Math.min(Number(query.limit ?? 100), 500);
    const offset = Math.max(Number(query.offset ?? 0), 0);
    const status = query.status && query.status !== "all" ? query.status : null;
    const { rows, total } = repo.list(status, limit, offset);
    return reply.send({ jobs: rows.map(mapJob), total, page: Math.floor(offset / limit) + 1, pageSize: limit });
  });

  app.post("/jobs", async (req, reply) => {
    const ctx = (app as any).ctx();
    const body = req.body as {
      type: string;
      payload: Record<string, unknown>;
      priority?: number;
      resourceClass?: string;
      clientName?: string;
      idempotencyKey?: string;
    };

    const jobId = globalThis.crypto?.randomUUID?.() ?? `job-${Date.now()}-${Math.random().toString(36).slice(2)}`;
    const repo = createJobRepository(ctx.db);
    repo.insert({
      id: jobId,
      type: body.type,
      priority: body.priority ?? 50,
      resourceClass: body.resourceClass ?? "none",
      clientName: body.clientName ?? null,
      requestPath: null,
      requestMethod: "POST",
      payload: body.payload,
      maxRetries: 2,
      idempotencyKey: body.idempotencyKey ?? null,
    });
    repo.insertEvent(jobId, "created", "Job created from dashboard/API", { type: body.type });

    ctx.queueStore.enqueue({ id: jobId, priority: body.priority ?? 50 });
    ctx.events.emit("job:created", { jobId, type: body.type });
    ctx.events.emit("queue:changed", ctx.queueStore.getSnapshot());

    return reply.send({
      jobId,
      status: "queued",
      position: 1,
    });
  });

  app.get("/jobs/:id", async (req, reply) => {
    const ctx = (app as any).ctx();
    const repo = createJobRepository(ctx.db);
    const job = repo.getById((req.params as { id: string }).id);
    if (!job) return reply.status(404).send({ error: "Job not found" });
    return reply.send(mapJob(job));
  });

  app.get("/jobs/:id/events", async (req, reply) => {
    const ctx = (app as any).ctx();
    const repo = createJobRepository(ctx.db);
    const events = repo.getEvents((req.params as { id: string }).id).map((event) => ({
      id: event.id,
      jobId: event.job_id,
      eventType: event.event_type,
      message: event.message,
      data: parseJson(event.data_json),
      createdAt: event.created_at,
    }));
    return reply.send({ events });
  });

  app.get("/jobs/:id/result", async (req, reply) => {
    const ctx = (app as any).ctx();
    const repo = createJobRepository(ctx.db);
    const result = repo.getResult((req.params as { id: string }).id);
    return reply.send({ result: result ?? null });
  });

  app.post("/jobs/:id/cancel", async (req, reply) => {
    const ctx = (app as any).ctx();
    const repo = createJobRepository(ctx.db);
    const id = (req.params as { id: string }).id;
    const job = repo.getById(id);
    if (!job) return reply.status(404).send({ error: "Job not found" });
    if (job.status === "succeeded" || job.status === "cancelled") {
      return reply.status(400).send({ error: `Cannot cancel job in state ${job.status}` });
    }
    repo.cancel(id);
    repo.insertEvent(id, "cancelled", "Job cancelled by user", {});
    ctx.queueStore.remove(id);
    ctx.events.emit("job:cancelled", { jobId: id });
    ctx.events.emit("queue:changed", ctx.queueStore.getSnapshot());
    return reply.send({ success: true, cancelled: true, jobId: id, job: mapJob(repo.getById(id)) });
  });

  app.post("/jobs/:id/retry", async (req, reply) => {
    const ctx = (app as any).ctx();
    const repo = createJobRepository(ctx.db);
    const id = (req.params as { id: string }).id;
    const job = repo.getById(id);
    if (!job) return reply.status(404).send({ error: "Job not found" });
    repo.retry(id);
    repo.insertEvent(id, "retried", "Job requeued by user", {});
    ctx.queueStore.enqueue({ id, priority: job.priority });
    ctx.events.emit("job:updated", { jobId: id, status: "queued" });
    ctx.events.emit("queue:changed", ctx.queueStore.getSnapshot());
    return reply.send({ success: true, retried: true, jobId: id, job: mapJob(repo.getById(id)) });
  });

  app.post("/jobs/:id/reprioritize", async (req, reply) => {
    const ctx = (app as any).ctx();
    const repo = createJobRepository(ctx.db);
    const id = (req.params as { id: string }).id;
    const body = req.body as { priority: number };
    const job = repo.getById(id);
    if (!job) return reply.status(404).send({ error: "Job not found" });
    repo.reprioritize(id, body.priority);
    repo.insertEvent(id, "reprioritized", "Job priority changed", { priority: body.priority });
    ctx.queueStore.enqueue({ id, priority: body.priority });
    ctx.events.emit("queue:changed", ctx.queueStore.getSnapshot());
    return reply.send({ success: true, reprioritized: true, jobId: id, priority: body.priority, job: mapJob(repo.getById(id)) });
  });
}
