import type { FastifyInstance } from "fastify";

export function registerHealthRoutes(app: FastifyInstance): void {
  app.get("/health", async (_request, reply) => {
    const ctx = (app as any).ctx() as { config: any; queueStore: any; ollama: any };
    const locked = ctx.queueStore.getLeased().length > 0;
    const ollama = ctx.ollama.getSnapshot();
    const degraded = ollama.status !== "running";
    return reply.send({
      status: degraded ? "degraded" : "healthy",
      reason: degraded ? `Ollama ${ollama.status}${ollama.lastError ? `: ${ollama.lastError}` : ""}` : null,
      uptime: process.uptime(),
      version: "0.1.0",
      timestamp: new Date().toISOString(),
      services: {
        gateway: "running",
        ollama: ollama.status,
      },
      queue: {
        gpuLocked: locked,
        lockedBy: ctx.queueStore.getLeased()[0]?.id ?? null,
      },
    });
  });
}
