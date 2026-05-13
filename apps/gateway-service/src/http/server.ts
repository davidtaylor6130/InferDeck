/**
 * HTTP server setup for the gateway service.
 * The dashboard/control app and Ollama-compatible proxy app are separate
 * listeners when configured to use different ports.
 */

import fastify, { type FastifyInstance } from "fastify";
import fastifyCors from "@fastify/cors";
import fastifyStatic from "@fastify/static";
import { existsSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { AppContext } from "../app";

import { registerHealthRoutes } from "./routes/health.routes";
import { registerStatusRoutes } from "./routes/status.routes";
import { registerEventsRoutes } from "./routes/events.routes";
import { registerQueueRoutes } from "./routes/queue.routes";
import { registerJobsRoutes } from "./routes/jobs.routes";
import { registerModelsRoutes } from "./routes/models.routes";
import { registerServicesRoutes } from "./routes/services.routes";
import { registerModesRoutes } from "./routes/modes.routes";
import { registerMetricsRoutes } from "./routes/metrics.routes";
import { registerHardwareRoutes } from "./routes/hardware.routes";
import { registerLogsRoutes } from "./routes/logs.routes";
import { registerProxyOllamaRoutes } from "./routes/proxy-ollama.routes";
import { registerProxyOpenAIRoutes } from "./routes/proxy-openai.routes";
import { requestIdPlugin } from "./middleware/requestId";
import { installAuthHook } from "./middleware/auth";
import { rateLimitPlugin } from "./middleware/rateLimit";
import { errorHandlerPlugin } from "./middleware/errorHandler";

const moduleDir = typeof import.meta.url === "string" ? dirname(fileURLToPath(import.meta.url)) : process.cwd();

function getExeDir(): string {
  return dirname(process.execPath);
}

function findPublicDir(): string {
  const exeDir = getExeDir();
  const possiblePaths = [
    join(process.cwd(), "public", "dashboard"),
    join(process.cwd(), "apps", "gateway-service", "public", "dashboard"),
    join(exeDir, "public", "dashboard"),
    (process as any).resourcesPath ? join((process as any).resourcesPath, "public", "dashboard") : "",
    join(moduleDir, "..", "public", "dashboard"),
  ];

  return possiblePaths.find((p) => p && existsSync(p)) ?? possiblePaths[0];
}

async function createBaseApp(ctx: AppContext): Promise<FastifyInstance> {
  const app = fastify({
    logger: {
      level: ctx.config.logging?.level ?? "info",
    },
  });

  app.decorate("ctx", () => ctx);
  app.decorate("config", () => ctx.config);

  await app.register(requestIdPlugin);
  await app.register(errorHandlerPlugin);
  await app.register(rateLimitPlugin);
  await app.register(fastifyCors, { origin: "*" });
  installAuthHook(app);

  return app;
}

export async function createDashboardApp(ctx: AppContext): Promise<FastifyInstance> {
  const app = await createBaseApp(ctx);

  await app.register(async (instance) => {
    installAuthHook(instance);
    registerHealthRoutes(instance);
    registerStatusRoutes(instance);
    registerEventsRoutes(instance);
    registerQueueRoutes(instance);
    registerJobsRoutes(instance);
    registerModelsRoutes(instance);
    registerServicesRoutes(instance);
    registerModesRoutes(instance);
    registerMetricsRoutes(instance);
    registerHardwareRoutes(instance);
    registerLogsRoutes(instance);
  }, { prefix: "/api" });

  const dashboardPath = findPublicDir();
  if (existsSync(dashboardPath)) {
    await app.register(fastifyStatic, {
      root: dashboardPath,
      prefix: "/",
      decorateReply: false,
    });
    app.log.info(`Dashboard served from ${dashboardPath}`);
  } else {
    app.get("/", async (_req, reply) => {
      return reply.type("text/html").send(
        "<!doctype html><title>InferDeck</title><h1>InferDeck</h1><p>Dashboard has not been built yet. Run pnpm --filter dashboard build.</p>"
      );
    });
    app.log.warn(`No dashboard found at ${dashboardPath}`);
  }

  return app;
}

export async function createProxyApp(ctx: AppContext): Promise<FastifyInstance> {
  const app = await createBaseApp(ctx);

  registerHealthRoutes(app);
  registerStatusRoutes(app);
  registerProxyOllamaRoutes(app);
  registerProxyOpenAIRoutes(app);

  return app;
}

export async function startServer(ctx: AppContext): Promise<void> {
  const proxyHost = ctx.config.server.proxyHost;
  const proxyPort = ctx.config.server.proxyPort;
  const dashboardHost = ctx.config.server.dashboardHost;
  const dashboardPort = ctx.config.server.dashboardPort;

  if (proxyPort === dashboardPort && proxyHost === dashboardHost) {
    const app = await createDashboardApp(ctx);
    registerProxyOllamaRoutes(app);
    registerProxyOpenAIRoutes(app);
    await app.listen({ port: proxyPort, host: proxyHost });
    app.log.info(`Gateway + Dashboard listening on http://${proxyHost}:${proxyPort}`);
    return;
  }

  const [proxyApp, dashboardApp] = await Promise.all([
    createProxyApp(ctx),
    createDashboardApp(ctx),
  ]);

  await Promise.all([
    proxyApp.listen({ port: proxyPort, host: proxyHost }),
    dashboardApp.listen({ port: dashboardPort, host: dashboardHost }),
  ]);

  proxyApp.log.info(`Gateway proxy API listening on http://${proxyHost}:${proxyPort}`);
  dashboardApp.log.info(`Dashboard control UI listening on http://${dashboardHost}:${dashboardPort}`);
  proxyApp.log.info(`Ollama: ${ctx.config.ollama.baseUrl}`);
  proxyApp.log.info(`Mode: ${ctx.config.modes.startupMode}`);
}
