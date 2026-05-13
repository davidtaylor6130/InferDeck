import type { FastifyInstance } from "fastify";

export function registerEventsRoutes(app: FastifyInstance): void {
  app.get("/events/stream", async (_request, reply) => {
    reply.header("Content-Type", "text/event-stream");
    reply.header("Cache-Control", "no-cache");
    reply.header("Connection", "keep-alive");
    (reply.raw as any).flush?.();

    const send = (event: string, data: any) => {
      reply.raw.write(`event: ${event}\n`);
      reply.raw.write(`data: ${JSON.stringify(data)}\n\n`);
      (reply.raw as any).flush?.();
    };

    const ctx = (app as any).ctx?.();
    const unlistenBus = ctx?.events?.subscribe((event: any) => send(event.type, event)) ?? (() => {});
    const unlistenQueue = ctx?.queueStore?.subscribe?.(() => send("queue:changed", { type: "queue:changed", data: ctx.queueStore.getSnapshot(), timestamp: new Date().toISOString() })) ?? (() => {});

    const heartbeat = setInterval(() => send("heartbeat", { timestamp: new Date().toISOString() }), 15000);
    heartbeat.unref?.();

    reply.raw.on("close", () => {
      clearInterval(heartbeat);
      unlistenBus();
      unlistenQueue();
    });

    send("connected", { type: "connected", data: {}, timestamp: new Date().toISOString() });
    return reply;
  });
}
