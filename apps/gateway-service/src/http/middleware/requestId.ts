import type { FastifyInstance, FastifyRequest, FastifyReply } from "fastify";

const requestIds = new WeakMap<FastifyRequest, string>();

export async function requestIdPlugin(app: FastifyInstance): Promise<void> {
  app.addHook("onRequest", async (req: any, reply: any) => {
    const id = req.headers["x-request-id"] as string;
    if (id) {
      requestIds.set(req, id);
      reply.header("X-Request-Id", id);
    } else {
      const generated = globalThis.crypto?.randomUUID?.() ?? `req-${Date.now()}-${Math.random().toString(36).slice(2)}`;
      requestIds.set(req, generated);
      reply.header("X-Request-Id", generated);
    }
  });

  app.decorate("getRequestId", (req: any) => requestIds.get(req) ?? "unknown");
}
