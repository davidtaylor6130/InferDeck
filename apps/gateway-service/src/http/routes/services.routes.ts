import type { FastifyInstance } from "fastify";

export function registerServicesRoutes(app: FastifyInstance): void {
  app.get("/services", async (_req, reply) => {
    const ctx = (app as any).ctx();
    const backend = ctx.backend.getSnapshot();
    const services = [
      {
        id: "gateway",
        name: "InferDeck Gateway",
        kind: "gateway",
        status: "running",
        pid: process.pid,
        baseUrl: ctx.config.server.publicBaseUrl,
        managed: true,
        version: "0.1.0",
        lastHealthcheckAt: new Date().toISOString(),
        lastError: null,
        updatedAt: new Date().toISOString(),
      },
      backend,
      ...ctx.managedServices.list(),
    ];

    return reply.send({
      services,
      healthyCount: services.filter((s) => s.status === "running").length,
      unhealthyCount: services.filter((s) => s.status !== "running").length,
    });
  });

  app.get("/services/llama-server/health", async (_req, reply) => {
    const ctx = (app as any).ctx();
    const health = await ctx.backend.checkHealth();
    return reply.send({
      service: ctx.backend.getSnapshot(),
      healthy: health.healthy,
      lastCheck: new Date().toISOString(),
      latencyMs: health.latencyMs,
      error: health.error,
    });
  });

  app.post("/services/llama-server/restart", async (_req, reply) => {
    const ctx = (app as any).ctx();
    if (!ctx.config.backend.managed || !ctx.backend.isManaged) {
      return reply.status(409).send({
        error: "llama.cpp is external or not managed by this gateway.",
        service: ctx.backend.getSnapshot(),
      });
    }
    const snap = await ctx.backend.restart();
    ctx.events.emit("service:health", snap);
    return reply.send({ restarted: true, service: snap });
  });

  app.post("/services/:id/start", async (req, reply) => {
    const ctx = (app as any).ctx();
    const id = (req.params as { id: string }).id;
    if (id === "llama-server") {
      await ctx.backend.start();
      return reply.send({ started: true, service: ctx.backend.getSnapshot() });
    }
    const service = ctx.managedServices.get(id);
    if (!service) return reply.status(404).send({ error: "Service not found" });
    return reply.send({ started: true, service: await service.start() });
  });

  app.post("/services/:id/stop", async (req, reply) => {
    const ctx = (app as any).ctx();
    const id = (req.params as { id: string }).id;
    if (id === "llama-server") {
      await ctx.backend.stop();
      return reply.send({ stopped: true, service: ctx.backend.getSnapshot() });
    }
    const service = ctx.managedServices.get(id);
    if (!service) return reply.status(404).send({ error: "Service not found" });
    return reply.send({ stopped: true, service: await service.stop() });
  });

  app.post("/services/:id/restart", async (req, reply) => {
    const ctx = (app as any).ctx();
    const id = (req.params as { id: string }).id;
    if (id === "llama-server") {
      const snap = await ctx.backend.restart();
      return reply.send({ restarted: true, service: snap });
    }
    const service = ctx.managedServices.get(id);
    if (!service) return reply.status(404).send({ error: "Service not found" });
    return reply.send({ restarted: true, service: await service.restart() });
  });

  app.get("/services/:id/health", async (req, reply) => {
    const ctx = (app as any).ctx();
    const id = (req.params as { id: string }).id;
    if (id === "llama-server") {
      const health = await ctx.backend.checkHealth();
      return reply.send({ service: ctx.backend.getSnapshot(), healthy: health.healthy, lastCheck: new Date().toISOString(), latencyMs: health.latencyMs, error: health.error });
    }
    const service = ctx.managedServices.get(id);
    if (!service) return reply.status(404).send({ error: "Service not found" });
    const healthy = await service.checkHealth();
    return reply.send({ service: service.getSnapshot(), healthy });
  });
}
