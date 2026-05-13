import type { FastifyInstance } from "fastify";

export function registerServicesRoutes(app: FastifyInstance): void {
  app.get("/services", async (_req, reply) => {
    const ctx = (app as any).ctx();
    const ollama = ctx.ollama.getSnapshot();
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
      ollama,
      ...ctx.managedServices.list(),
    ];

    return reply.send({
      services,
      healthyCount: services.filter((service) => service.status === "running").length,
      unhealthyCount: services.filter((service) => service.status !== "running").length,
    });
  });

  app.get("/services/ollama/health", async (_req, reply) => {
    const ctx = (app as any).ctx();
    const health = await ctx.ollama.checkHealth();
    return reply.send({
      service: ctx.ollama.getSnapshot(),
      healthy: health.healthy,
      lastCheck: new Date().toISOString(),
      latencyMs: health.latencyMs,
      error: health.error,
    });
  });

  app.post("/services/ollama/restart", async (_req, reply) => {
    const ctx = (app as any).ctx();
    if (!ctx.config.ollama.manageProcess || !ctx.ollama.isManaged) {
      return reply.status(409).send({
        error: "Ollama is external or not managed by this gateway.",
        service: ctx.ollama.getSnapshot(),
      });
    }
    const service = await ctx.ollama.restart();
    ctx.events.emit("service:health", service);
    return reply.send({ restarted: true, service });
  });

  app.post("/services/:id/start", async (req, reply) => {
    const ctx = (app as any).ctx();
    const id = (req.params as { id: string }).id;
    if (id === "ollama") {
      await ctx.ollama.start();
      return reply.send({ started: true, service: ctx.ollama.getSnapshot() });
    }
    const service = ctx.managedServices.get(id);
    if (!service) return reply.status(404).send({ error: "Service not found" });
    return reply.send({ started: true, service: await service.start() });
  });

  app.post("/services/:id/stop", async (req, reply) => {
    const ctx = (app as any).ctx();
    const id = (req.params as { id: string }).id;
    if (id === "ollama") {
      await ctx.ollama.stop();
      return reply.send({ stopped: true, service: ctx.ollama.getSnapshot() });
    }
    const service = ctx.managedServices.get(id);
    if (!service) return reply.status(404).send({ error: "Service not found" });
    return reply.send({ stopped: true, service: await service.stop() });
  });

  app.post("/services/:id/restart", async (req, reply) => {
    const ctx = (app as any).ctx();
    const id = (req.params as { id: string }).id;
    if (id === "ollama") {
      const service = await ctx.ollama.restart();
      return reply.send({ restarted: true, service });
    }
    const service = ctx.managedServices.get(id);
    if (!service) return reply.status(404).send({ error: "Service not found" });
    return reply.send({ restarted: true, service: await service.restart() });
  });

  app.get("/services/:id/health", async (req, reply) => {
    const ctx = (app as any).ctx();
    const id = (req.params as { id: string }).id;
    if (id === "ollama") {
      const health = await ctx.ollama.checkHealth();
      return reply.send({ service: ctx.ollama.getSnapshot(), healthy: health.healthy, lastCheck: new Date().toISOString(), latencyMs: health.latencyMs, error: health.error });
    }
    const service = ctx.managedServices.get(id);
    if (!service) return reply.status(404).send({ error: "Service not found" });
    const healthy = await service.checkHealth();
    return reply.send({ service: service.getSnapshot(), healthy });
  });
}
