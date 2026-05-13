import type { FastifyInstance } from "fastify";

export function registerHardwareRoutes(app: FastifyInstance): void {
  app.get("/hardware", async (_req, reply) => {
    const ctx = (app as any).ctx();
    return reply.send(ctx.hardware.getSnapshot());
  });

  app.post("/hardware/poll", async (_req, reply) => {
    const ctx = (app as any).ctx();
    return reply.send(await ctx.hardware.poll());
  });
}
