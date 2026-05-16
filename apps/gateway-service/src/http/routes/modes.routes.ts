import type { FastifyInstance } from "fastify";

export function registerModesRoutes(app: FastifyInstance): void {
  app.get("/modes", async (_req, reply) => {
    const ctx = (app as any).ctx();
    const mode = ctx.scheduler.getMode();
    const lockManager = ctx.lockManager;
    return reply.send({
      mode,
      gamingMode: lockManager.isGamingModeActive()
        ? {
            active: true,
            rejectedInteractiveLlm: lockManager.shouldRejectInteractiveLlm(),
            pausedBackgroundJobs: lockManager.shouldPauseBackgroundJobs(),
            unloadBackendModels: lockManager.shouldUnloadBackendModels(),
          }
        : { active: false },
    });
  });

  app.post("/modes/ai", async (_req, reply) => {
    const ctx = (app as any).ctx();
    ctx.scheduler.setMode("ai");
    ctx.lockManager.setGamingMode({ ...ctx.lockManager.getGamingMode(), active: false });
    return reply.send({ mode: "ai" });
  });

  app.post("/modes/gaming", async (_req, reply) => {
    const ctx = (app as any).ctx();
    ctx.scheduler.setMode("gaming");
    ctx.lockManager.setGamingMode({ active: true, ...ctx.config.modes.gamingMode });
    return reply.send({ mode: "gaming" });
  });

  app.post("/modes/maintenance", async (_req, reply) => {
    const ctx = (app as any).ctx();
    ctx.scheduler.setMode("maintenance");
    return reply.send({ mode: "maintenance" });
  });
}
