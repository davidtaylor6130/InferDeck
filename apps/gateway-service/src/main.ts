/**
 * Gateway service entry point.
 * - Loads config
 * - Boots SQLite, scheduler, proxy adapters
 * - Starts Fastify on dashboard (8721) and proxy (11434)
 */

import { existsSync } from "node:fs";
import { resolve, dirname, join } from "node:path";
import { loadConfig } from "./config/loadConfig";
import { startServer } from "./http/server";
import { AppContext } from "./app";
import type { FastifyInstance } from "fastify";
import { writeFileSync, mkdirSync } from "node:fs";

let ctx: AppContext | null = null;
let servers: FastifyInstance[] = [];
let shuttingDown = false;

function parseConfigArg(): string {
  const idx = process.argv.findIndex((a) => a === "--config");
  let configFile = "gateway.local.yaml";

  if (idx !== -1 && process.argv[idx + 1]) {
    configFile = process.argv[idx + 1];
  }

  const possiblePaths = [
    configFile,
    resolve(process.cwd(), "config", configFile),
    resolve(process.cwd(), configFile),
    resolve(dirname(process.execPath), "config", configFile),
    (process as any).resourcesPath ? resolve((process as any).resourcesPath, "config", configFile) : "",
  ];

  for (const p of possiblePaths) {
    if (p && existsSync(p)) {
      console.log(`Found config at: ${p}`);
      return p;
    }
  }

  return possiblePaths[0];
}

function writeCrashReport(type: string, err: unknown, extra?: Record<string, unknown>): void {
  try {
    const logDir = process.env.R9700_LOG_DIR || join(process.cwd(), "data", "logs");
    const crashDir = join(logDir, "crash-reports");
    mkdirSync(crashDir, { recursive: true });
    const timestamp = new Date().toISOString().replace(/[:.]/g, "-");
    const filePath = join(crashDir, `crash-${timestamp}.json`);
    const report = {
      type,
      timestamp: new Date().toISOString(),
      uptime: process.uptime(),
      pid: process.pid,
      platform: process.platform,
      nodeVersion: process.version,
      memoryUsage: process.memoryUsage(),
      error: err instanceof Error ? {
        message: err.message,
        stack: err.stack,
        name: err.name,
      } : { message: String(err) },
      extra,
    };
    writeFileSync(filePath, JSON.stringify(report, null, 2));
    console.error(`[crash] Report written to ${filePath}`);
  } catch {
    // Best effort — if we can't write the report, just log to stderr
  }
}

function installCrashHandlers(): void {
  process.on("uncaughtException", (err, origin) => {
    console.error(`[crash] Uncaught exception (${origin}):`, err);
    writeCrashReport("uncaughtException", err, { origin });
    if (!shuttingDown) void gracefulShutdown(1);
  });

  process.on("unhandledRejection", (reason, promise) => {
    console.error("[crash] Unhandled rejection:", reason);
    writeCrashReport("unhandledRejection", reason, { promise: String(promise) });
    if (!shuttingDown) void gracefulShutdown(1);
  });

  // Handle SIGINT/SIGTERM for graceful shutdown
  process.on("SIGINT", () => void gracefulShutdown(0));
  process.on("SIGTERM", () => void gracefulShutdown(0));
}

async function gracefulShutdown(exitCode: number): Promise<void> {
  if (shuttingDown) return;
  shuttingDown = true;

  console.log(`[shutdown] Starting graceful shutdown (exitCode=${exitCode})...`);

  // Phase 1: Close HTTP servers (stop accepting new connections)
  const closePromises = servers.map((s) =>
    s.close().catch((err) => console.error(`[shutdown] Error closing server:`, err))
  );
  await Promise.allSettled(closePromises);
  console.log("[shutdown] HTTP servers closed");

  // Phase 2: Shutdown app context (DB, backend, services)
  if (ctx) {
    await ctx.shutdown().catch((err) => console.error(`[shutdown] Error in ctx.shutdown():`, err));
    console.log("[shutdown] App context shut down");
  }

  // Phase 3: Force exit after timeout
  const forceExit = setTimeout(() => {
    console.error("[shutdown] Force exit after timeout");
    process.exit(1);
  }, 10000);
  forceExit.unref();

  process.exit(exitCode);
}

async function main(): Promise<void> {
  installCrashHandlers();

  const configPath = parseConfigArg();
  let configObj;
  try {
    configObj = loadConfig(configPath);
    console.log(`Config loaded from ${configPath}`);
  } catch (err: any) {
    console.error("Failed to load config:", err.message);
    process.exit(1);
  }

  // Startup timeout: if init takes longer than 60s, exit so supervisor restarts
  const startupTimeout = setTimeout(() => {
    console.error("[startup] Startup timed out after 60s — exiting for supervisor restart");
    writeCrashReport("startupTimeout", new Error("Startup timed out after 60s"));
    process.exit(1);
  }, 60000);

  try {
    ctx = new AppContext(configObj);
    await ctx.init();
    const startedServers = await startServer(ctx);
    servers = startedServers;
    console.log("[startup] Gateway is ready and accepting connections");
  } catch (err: any) {
    clearTimeout(startupTimeout);
    console.error("Startup error:", err);
    writeCrashReport("startupError", err);
    process.exit(1);
  }

  clearTimeout(startupTimeout);
}

main().catch((err) => {
  console.error("Fatal startup error:", err);
  writeCrashReport("fatalStartupError", err);
  process.exit(1);
});
