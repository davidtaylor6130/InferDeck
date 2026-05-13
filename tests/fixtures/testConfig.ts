/**
 * Integration test fixtures for the gateway service
 */

import type { GatewayConfig } from "../../apps/gateway-service/src/config/schema";

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
  ollama: {
    enabled: true,
    baseUrl: "http://127.0.0.1:11436",
    manageProcess: false,
    executable: "",
    healthcheckIntervalMs: 1000,
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
      unloadOllamaModels: true,
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
