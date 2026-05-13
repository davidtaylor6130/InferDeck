/**
 * Gateway config loader with YAML parsing and schema validation.
 */

import { readFileSync, existsSync } from "node:fs";
import { join } from "node:path";
import { parse } from "yaml";
import { configSchema, type GatewayConfigRaw, type GatewayConfig } from "./schema";

function findConfigPath(projectRoot: string, envPath: string): string {
  const localPath = join(projectRoot, "config", envPath);
  const examplePath = join(projectRoot, "config", "gateway.example.yaml");

  if (existsSync(localPath)) {
    return localPath;
  }

  if (existsSync(examplePath)) {
    console.warn("Config not found, using gateway.example.yaml as fallback");
    return examplePath;
  }

  throw new Error(
    `No config file found. Expected ${localPath} or ${examplePath}`
  );
}

export function loadConfig(envPath: string = "config/gateway.local.yaml"): GatewayConfig {
  const projectRoot = process.cwd();
  
  // If the path is absolute or exists as-is, use it directly
  if (existsSync(envPath)) {
    const configPath = envPath;
    const rawText = readFileSync(configPath, "utf-8");
    const raw: GatewayConfigRaw = parse(rawText);
    const result = configSchema.safeParse(raw);
    if (!result.success) {
      const errors = result.error.issues.map((i) => `${i.path.join(".")}: ${i.message}`).join(", ");
      throw new Error(`Config validation failed: ${errors}`);
    }
    const config = result.data;
    const dbPath = process.env.R9700_DB_PATH;
    if (dbPath) config.database.path = dbPath;
    const logDir = process.env.R9700_LOG_DIR;
    if (logDir) config.logging.dir = logDir;
    const apiKeyEnvName = process.env.R9700_API_KEY_ENV;
    if (apiKeyEnvName) config.security.apiKeyEnv = apiKeyEnvName;
    return config;
  }
  
  const configPath = findConfigPath(projectRoot, envPath);
  const rawText = readFileSync(configPath, "utf-8");
  const raw: GatewayConfigRaw = parse(rawText);

  const result = configSchema.safeParse(raw);
  if (!result.success) {
    const errors = result.error.issues.map((i) => `${i.path.join(".")}: ${i.message}`).join(", ");
    throw new Error(`Config validation failed: ${errors}`);
  }

  const config = result.data;

  // Apply environment variable overrides
  const dbPath = process.env.R9700_DB_PATH;
  if (dbPath) config.database.path = dbPath;

  const logDir = process.env.R9700_LOG_DIR;
  if (logDir) config.logging.dir = logDir;

  const apiKeyEnvName = process.env.R9700_API_KEY_ENV;
  if (apiKeyEnvName) config.security.apiKeyEnv = apiKeyEnvName;

  return config;
}
