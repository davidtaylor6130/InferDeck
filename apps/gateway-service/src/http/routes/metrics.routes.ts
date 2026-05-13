import type { FastifyInstance } from "fastify";

export function registerMetricsRoutes(app: FastifyInstance): void {
  app.get("/metrics", async (_req, reply) => {
    const ctx = (app as any).ctx();
    const snapshot = ctx.queueStore.getSnapshot();
    return reply.send({
      queueLength: snapshot.queued,
      running: snapshot.leased,
      paused: snapshot.paused,
      gpuLocked: snapshot.gpuLocked,
      gpuLockedBy: snapshot.lockedBy,
      estimatedWaitMs: snapshot.leased * 5000,
      samples: ctx.metrics.list(Number((_req.query as any)?.minutes ?? 60)),
      hardware: ctx.hardware.getSnapshot(),
      timestamp: new Date().toISOString(),
    });
  });
}
