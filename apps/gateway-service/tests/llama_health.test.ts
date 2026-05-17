import { test, expect } from "vitest";
import fastify from "fastify";
import { registerHealthRoutes } from "../src/http/routes/health.routes";

// Minimal mock of the backend used by the gateway.
class MockBackend {
  getSnapshot() {
    return {
      status: "running",
      pid: 123,
      baseUrl: "http://127.0.0.1:11434",
      managed: true,
      model: "mock-model.gguf",
      version: "0.1.0",
      lastHealthcheckAt: "2026-05-17T00:00:00Z",
      lastError: null,
      updatedAt: "2026-05-17T00:00:00Z",
    };
  }
  async checkHealth() {
    return { healthy: true, latencyMs: 10, error: null };
  }
}

test("/llama-health returns backend snapshot", async () => {
  const app = fastify();
  const backend = new MockBackend();
  // The health routes expect a ctx() function returning an object.
  app.decorate("ctx", () => ({ backend }));
  registerHealthRoutes(app);
  const res = await app.inject({ method: "GET", url: "/llama-health" });
  expect(res.statusCode).toBe(200);
  const body = JSON.parse(res.payload);
  expect(body).toMatchObject({
    status: "running",
    pid: 123,
    baseUrl: "http://127.0.0.1:11434",
    managed: true,
    model: "mock-model.gguf",
    version: "0.1.0",
    lastHealthcheckAt: "2026-05-17T00:00:00Z",
    lastError: null,
    updatedAt: "2026-05-17T00:00:00Z",
  });
});

