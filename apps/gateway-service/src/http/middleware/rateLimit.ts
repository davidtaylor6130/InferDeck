import type { FastifyInstance, FastifyRequest, FastifyReply } from "fastify";

interface RateLimitOptions {
  maxRequests: number;
  windowMs: number;
  cleanupIntervalMs?: number;
  maxEntries?: number;
}

function createRateLimiter(options: RateLimitOptions) {
  const windows = new Map<string, { count: number; startTime: number }>();

  // Periodic cleanup to prevent memory leaks over long-running operation
  const cleanupInterval = options.cleanupIntervalMs ?? 60000;
  const maxEntries = options.maxEntries ?? 10000;

  const cleanupTimer = setInterval(() => {
    const now = Date.now();
    const expired = new Set<string>();

    for (const [ip, window] of windows.entries()) {
      if (now - window.startTime > options.windowMs) {
        expired.add(ip);
      }
    }

    expired.forEach((ip) => windows.delete(ip));

    // If still over limit, remove oldest entries
    if (windows.size > maxEntries) {
      const sorted = Array.from(windows.entries())
        .sort((a, b) => a[1].startTime - b[1].startTime);
      const toRemove = sorted.slice(0, windows.size - maxEntries);
      toRemove.forEach(([ip]) => windows.delete(ip));
    }
  }, cleanupInterval);

  // Don't keep process alive just for cleanup timer
  cleanupTimer.unref();

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
