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

  /**
   * /ready — for process supervisors (WinSW, systemd, Kubernetes)
   * Returns 200 only when all critical subsystems are operational.
   * Returns 503 if any critical subsystem is down.
   */
  app.get("/ready", async (_request, reply) => {
    try {
      const ctx = (app as any).ctx() as {
        db: { client: any };
        backend: { getSnapshot: () => { status: string } };
      };

      const checks: Record<string, boolean> = {
        database: false,
        llama_cpp: false,
      };

      // Check database
      try {
        ctx.db.client.exec("SELECT 1");
        checks.database = true;
      } catch {
        checks.database = false;
      }

      // Check backend
      try {
        const snap = ctx.backend.getSnapshot();
        checks.llama_cpp = snap.status === "running";
      } catch {
        checks.llama_cpp = false;
      }

      const allHealthy = Object.values(checks).every(Boolean);

      if (!allHealthy) {
        return reply.status(503).send({
          status: "not_ready",
          checks,
          timestamp: new Date().toISOString(),
        });
      }

      return reply.send({
        status: "ready",
        checks,
        timestamp: new Date().toISOString(),
      });
    } catch (err: any) {
      return reply.status(503).send({
        status: "not_ready",
        error: err.message,
        timestamp: new Date().toISOString(),
      });
    }
  });
}
