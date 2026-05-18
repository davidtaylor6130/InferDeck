/**
 * Zod config schema for gateway settings.
 */

import { z } from "zod";

export const configSchema = z.object({
  server: z.object({
    dashboardHost: z.string().default("0.0.0.0"),
    dashboardPort: z.number().default(8721),
    proxyHost: z.string().default("0.0.0.0"),
    proxyPort: z.number().default(11434),
    publicBaseUrl: z.string().url().default("http://localhost:8721"),
  }).default({}),
  security: z.object({
    requireApiKey: z.boolean().default(true),
    apiKeyEnv: z.string().default("R9700_GATEWAY_ADMIN_KEY"),
    allowedLanCidrs: z.array(z.string()).default([
      "192.168.0.0/16",
      "10.0.0.0/8",
      "172.16.0.0/12",
      "100.64.0.0/10",
    ]),
  }).default({}),
  database: z.object({
    path: z.string().default("./data/gateway.sqlite"),
    walMode: z.boolean().default(true),
  }).default({}),
  backend: z.object({
    enabled: z.boolean().default(true),
    baseUrl: z.string().url().default("http://127.0.0.1:11434"),
    managed: z.boolean().default(true),
    executable: z.string().default("llama-server"),
    ggufDirectory: z.string().default("./models"),
    model: z.string().nullable().default(null),
    bindHost: z.string().default("127.0.0.1"),
    bindPort: z.number().default(11434),
    maxGpuLayers: z.number().default(999),
    noKvOffload: z.boolean().default(false),
    ctxSize: z.number().default(100000),
    flashAttn: z.boolean().default(true),
    cacheTypeK: z.string().default("q8_0"),
    cacheTypeV: z.string().default("q8_0"),
    mmap: z.boolean().default(false),
    healthcheckIntervalMs: z.number().default(10000),
    restartOnFailure: z.boolean().default(true),
  }).default({}),
  scheduler: z.object({
    maxConcurrentGpuHeavyJobs: z.number().default(1),
    maxHiddenInteractiveWaitMs: z.number().default(20000),
    defaultRetryAfterSeconds: z.number().default(30),
    staleRunningJobAfterMs: z.number().default(600000),
    heartbeatIntervalMs: z.number().default(5000),
    jobLeaseSeconds: z.number().default(120),
    maxQueueSize: z.number().default(1000),
  }).default({}),
  modes: z.object({
    startupMode: z.enum(["ai", "gaming", "maintenance"]).default("ai"),
    gamingMode: z.object({
      rejectInteractiveLlm: z.boolean().default(true),
      pauseBackgroundJobs: z.boolean().default(true),
      unloadBackendModels: z.boolean().default(true),
      stopComfyUi: z.boolean().default(true),
    }).default({}),
  }).default({}),
  hardware: z.object({
    provider: z.string().default("null"),
    pollIntervalMs: z.number().default(2000),
    helperPath: z.string().optional(),
  }).default({}),
  managedServices: z.array(z.object({
    id: z.string(),
    name: z.string(),
    kind: z.string(),
    enabled: z.boolean().default(false),
    command: z.string().default(""),
    args: z.array(z.string()).default([]),
    cwd: z.string().optional(),
    env: z.record(z.string()).default({}),
    baseUrl: z.string().nullable().optional(),
    healthUrl: z.string().nullable().optional(),
    startupTimeoutMs: z.number().default(15000),
    restartOnFailure: z.boolean().default(false),
  })).default([]),
  logging: z.object({
    level: z.enum(["trace", "debug", "info", "warn", "error", "fatal"]).default("info"),
    dir: z.string().default("./data/logs"),
    retentionDays: z.number().default(30),
  }).default({}),
});

export type GatewayConfigRaw = z.input<typeof configSchema>;
export type GatewayConfig = z.output<typeof configSchema>;
