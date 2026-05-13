/**
 * Gateway service entry point.
 * - Loads config
 * - Boots SQLite, scheduler, proxy adapters
 * - Starts Fastify on dashboard (8721) and proxy (11434)
 */

import { existsSync } from "node:fs";
import { resolve, dirname } from "node:path";
import { loadConfig } from "./config/loadConfig";
import { startServer } from "./http/server";
import { AppContext } from "./app";

let ctx: AppContext | null = null;

function parseConfigArg(): string {
  const idx = process.argv.findIndex((a) => a === "--config");
  let configFile = "gateway.local.yaml";

  if (idx !== -1 && process.argv[idx + 1]) {
    configFile = process.argv[idx + 1];
  }

  // Try multiple possible locations
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

async function main(): Promise<void> {
  const configPath = parseConfigArg();
  let configObj;
  try {
    configObj = loadConfig(configPath);
    console.log(`Config loaded from ${configPath}`);
  } catch (err: any) {
    console.error("Failed to load config:", err.message);
    process.exit(1);
  }

  ctx = new AppContext(configObj);
  await ctx.init();
  await startServer(ctx);
}

main().catch((err) => {
  console.error("Startup error:", err);
  process.exit(1);
});

async function shutdown(): Promise<void> {
  if (ctx) await ctx.shutdown();
  process.exit(0);
}

process.on("SIGINT", () => void shutdown());
process.on("SIGTERM", () => void shutdown());
