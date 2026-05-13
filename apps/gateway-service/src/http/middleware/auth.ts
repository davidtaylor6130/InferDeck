import type { FastifyRequest, FastifyReply, FastifyInstance } from "fastify";

export async function authPlugin(app: FastifyInstance): Promise<void> {
  installAuthHook(app);
}

export function installAuthHook(app: FastifyInstance): void {
  const config: any = (app as any).ctx?.()?.config ?? (app as any).config?.() ?? {};
  requireApiKey = config.security?.requireApiKey ?? false;
  apiKeyEnv = config.security?.apiKeyEnv ?? "R9700_GATEWAY_ADMIN_KEY";

  const validKey = process.env[apiKeyEnv] || "";

  app.addHook("onRequest", async (req: any, reply: any) => {
    const path = req.url.split("?")[0];

    if (path === "/health" || path === "/status" || path === "/api/health" || path === "/api/status") return;
    if (path === "/events/stream" || path === "/api/events/stream") return;
    const protectedControlPath = [
      "/queue",
      "/jobs",
      "/models",
      "/services",
      "/modes",
      "/metrics",
      "/hardware",
      "/logs",
    ].some((prefix) => path === prefix || path.startsWith(`${prefix}/`));
    if (!path.startsWith("/api") && !path.startsWith("/v1") && !protectedControlPath) return;

    const providedKey = req.headers["x-api-key"] ?? req.headers["authorization"] ?? "";
    const key = providedKey.startsWith("Bearer ") ? providedKey.slice(7) : providedKey;

    if (!requireApiKey || !validKey) return;

    if (!key || key !== validKey) {
      return reply.status(401).send({
        error: { type: "unauthorized", message: "Missing or invalid API key" },
      });
    }
  });

  app.decorate("getAuthConfig", () => ({
    requireApiKey,
    apiKeyEnv,
  }));
}

let requireApiKey = false;
let apiKeyEnv = "R9700_GATEWAY_ADMIN_KEY";
