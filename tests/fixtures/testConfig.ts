/**
 * Test setup and fixtures
 */

import { beforeAll } from "vitest";
import type { GatewayConfig } from "../../apps/gateway-service/src/config/schema";

beforeAll(() => {
  // QueueStore uses no external dependencies or environment variables,
  // so no global mocks are required.
});

// Test configuration
export const testConfig: GatewayConfig = {
  server: {
    dashboardHost: "127.0.0.1",
    dashboardPort: 18721,
    proxyHost: "127.0.0.1",
    proxyPort: 11435,
    publicBaseUrl: "http://127.0.0.1:18721",
  },
  security: {
    requireApiKey: false,
    apiKeyEnv: "TEST_API_KEY",
    allowedLanCidrs: ["127.0.0.0/8"],
  },
  database: {
    path: "./data/test-gateway.sqlite",
    walMode: true,
  },
  backend: {
    enabled: true,
    baseUrl: "http://127.0.0.1:11434",
    managed: false,
    executable: "",
    ggufDirectory: "./models",
    model: null,
    bindHost: "127.0.0.1",
    bindPort: 11434,
    maxGpuLayers: 999,
    noKvOffload: true,
    ctxSize: 100000,
    flashAttn: true,
    cacheTypeK: "q8_0",
    cacheTypeV: "q8_0",
    mmap: false,
    healthcheckIntervalMs: 10000,
    restartOnFailure: false,
  },
  scheduler: {
    maxConcurrentGpuHeavyJobs: 1,
    maxHiddenInteractiveWaitMs: 5000,
    defaultRetryAfterSeconds: 5,
    staleRunningJobAfterMs: 10000,
    heartbeatIntervalMs: 1000,
    jobLeaseSeconds: 30,
  },
  modes: {
    startupMode: "ai",
    gamingMode: {
      rejectInteractiveLlm: true,
      pauseBackgroundJobs: true,
      unloadBackendModels: true,
      stopComfyUi: true,
    },
  },
  hardware: {
    provider: "null",
    pollIntervalMs: 1000,
  },
  managedServices: [],
  logging: {
    level: "warn",
    dir: "./data/test-logs",
    retentionDays: 1,
  },
};

// Test database path
export const testDbPath = "./data/test-gateway.sqlite";

// Test API key
export const testApiKey = "test-secret-key";
