import type { FastifyInstance } from "fastify";

export function registerHealthRoutes(app: FastifyInstance): void {
  app.get("/health", async (_request, reply) => {
    const ctx = (app as any).ctx() as { config: any; queueStore: any; backend: any };
    const locked = ctx.queueStore.getLeased().length > 0;
    const backend = ctx.backend.getSnapshot();
    const degraded = backend.status !== "running";
    return reply.send({
      status: degraded ? "degraded" : "healthy",
      reason: degraded ? `llama.cpp ${backend.status}${backend.lastError ? `: ${backend.lastError}` : ""}` : null,
      uptime: process.uptime(),
      version: "0.1.0",
      timestamp: new Date().toISOString(),
      services: {
        gateway: "running",
        llama_cpp: backend.status,
      },
      queue: {
        gpuLocked: locked,
        lockedBy: ctx.queueStore.getLeased()[0]?.id ?? null,
      },
    });
  });
}
