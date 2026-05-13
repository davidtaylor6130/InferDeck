import type { FastifyInstance } from "fastify";
import { createJobRepository } from "../../db/repositories/jobs.repo";

export function registerQueueRoutes(app: FastifyInstance): void {
  app.get("/queue", async (_request, reply) => {
    const ctx = (app as any).ctx();
    const repo = createJobRepository(ctx.db);
    const { rows } = repo.list(null, 250, 0);
    return reply.send({
      snapshot: ctx.queueStore.getSnapshot(),
      jobs: ctx.queueStore.getQueued(50, 0),
      allJobs: rows,
    });
  });

  app.post("/queue/pause", async (_request, reply) => {
    const ctx = (app as any).ctx();
    const count = ctx.queueStore.pauseAll();
    const repo = createJobRepository(ctx.db);
    for (const item of ctx.queueStore.getPaused()) repo.updateStatus(item.id, "paused");
    ctx.events.emit("queue:changed", ctx.queueStore.getSnapshot());
    return reply.send({ paused: count });
  });

  app.post("/queue/resume", async (_request, reply) => {
    const ctx = (app as any).ctx();
    const count = ctx.queueStore.resumeAll();
    const repo = createJobRepository(ctx.db);
    for (const item of ctx.queueStore.getQueued(500, 0)) repo.updateStatus(item.id, "queued");
    ctx.events.emit("queue:changed", ctx.queueStore.getSnapshot());
    return reply.send({ resumed: count });
  });

  app.post("/queue/clear-failed", async (_request, reply) => {
    const ctx = (app as any).ctx();
    const count = ctx.queueStore.clearFailed();
    const repo = createJobRepository(ctx.db);
    const { rows } = repo.list("failed", 1000, 0);
    for (const row of rows) repo.delete(row.id);
    const dead = repo.list("dead_letter", 1000, 0).rows;
    for (const row of dead) repo.delete(row.id);
    ctx.events.emit("queue:changed", ctx.queueStore.getSnapshot());
    return reply.send({ cleared: count + rows.length + dead.length });
  });
}
