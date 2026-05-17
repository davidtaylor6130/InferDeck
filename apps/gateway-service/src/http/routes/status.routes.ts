import type { FastifyInstance } from "fastify";

export function registerStatusRoutes(app: FastifyInstance): void {
  app.get("/status", async (_request, reply) => {
    const ctx = (app as any).ctx();
    const queueSnapshot = ctx.queueStore.getSnapshot();
    const schedulerMode = ctx.scheduler.getMode();
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
    const hardware = ctx.hardware.getSnapshot();
    const logs = ctx.logs.stats();
    const activeJob = ctx.queueStore.getLeased()[0] ?? null;
    return reply.send({
      health: backend.status === "running" ? "healthy" : "degraded",
      mode: {
        mode: schedulerMode,
        gamingMode: ctx.lockManager.isGamingModeActive() ? { active: true } : { active: false },
      },
      queue: {
        ...queueSnapshot,
        totalQueued: queueSnapshot.queued,
        totalRunning: queueSnapshot.leased,
        totalPaused: queueSnapshot.paused,
        totalFailed: 0,
        totalDeadLetter: ctx.queueStore.getDeadLetter().length,
        estimatedWaitMs: queueSnapshot.leased * 5000,
      },
      activeWorkload: activeJob,
      hardware,
      metricsSamples: ctx.metrics.list(60),
      storage: {
        dataDirectory: ctx.config.database.path,
        logsDirectory: logs.dir,
        logSizeBytes: logs.totalBytes,
        storageMode: ctx.config.database.walMode ? "SQLite WAL" : "SQLite",
      },
      uptimeMs: process.uptime() * 1000,
      config: {
        dashboard: `${ctx.config.server.publicBaseUrl}`,
        proxy: `${ctx.config.server.proxyHost}:${ctx.config.server.proxyPort}`,
        backend: ctx.config.backend.baseUrl,
      },
      services,
    });
  });
}
