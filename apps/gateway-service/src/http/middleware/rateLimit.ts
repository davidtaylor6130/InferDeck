import type { FastifyInstance, FastifyRequest, FastifyReply } from "fastify";

interface RateLimitOptions {
  maxRequests: number;
  windowMs: number;
}

function createRateLimiter(options: RateLimitOptions) {
  const windows = new Map<string, { count: number; startTime: number }>();

  return function isRateLimited(req: FastifyRequest): boolean {
    const ip = req.ip ?? req.socket.remoteAddress ?? "unknown";
    const now = Date.now();
    const window = windows.get(ip);

    if (!window || now - window.startTime > options.windowMs) {
      windows.set(ip, { count: 1, startTime: now });
      return false;
    }

    window.count++;
    if (window.count > options.maxRequests) {
      return true;
    }

    return false;
  };
}

export async function rateLimitPlugin(app: FastifyInstance): Promise<void> {
  const limiter = createRateLimiter({
    maxRequests: 100,
    windowMs: 60000,
  });

  app.addHook("onRequest", async (req: any, reply: any) => {
    if (limiter(req)) {
      reply.status(429).send({
        error: {
          type: "rate_limit",
          message: "Too many requests. Please try again later.",
          retryAfter: 60,
        },
      });
      return;
    }
  });
}
