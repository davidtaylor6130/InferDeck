/**
 * Integration test for the gateway service
 */

import { describe, it, expect, beforeAll, afterAll, vi } from "vitest";
import type { FastifyInstance } from "fastify";
import { rmSync } from "node:fs";
import { AppContext } from "../../apps/gateway-service/src/app";
import { createDashboardApp, createProxyApp } from "../../apps/gateway-service/src/http/server";
import { testApiKey, testConfig } from "../fixtures/testConfig";

describe("Gateway Service Integration", () => {
  let app: FastifyInstance;
  let ctx: AppContext;

  beforeAll(async () => {
    process.env.TEST_API_KEY = testApiKey;
    ctx = new AppContext({
      ...testConfig,
      security: { ...testConfig.security, requireApiKey: true },
    });
    app = await createDashboardApp(ctx);
  });

  afterAll(async () => {
    await app.close();
    await ctx.shutdown();
  });

  it("should respond to health check", async () => {
    const res = await app.inject({
      method: "GET",
      url: "/api/health",
    });
    expect(res.statusCode).toBe(200);
    const body = JSON.parse(res.body);
    expect(["healthy", "degraded"]).toContain(body.status);
  });

  it("should reject invalid API key", async () => {
    const res = await app.inject({
      method: "GET",
      url: "/api/jobs",
      headers: { "x-api-key": "wrong-key" },
    });
    expect(res.statusCode).toBe(401);
  });
});

describe("Gateway proxy queue integration", () => {
  let app: FastifyInstance;
  let ctx: AppContext;
  const originalFetch = globalThis.fetch;

  beforeAll(async () => {
    globalThis.fetch = vi.fn(async (url: string | URL, init?: RequestInit) => {
      const href = String(url);
      if (href.endsWith("/health")) {
        return Response.json({ status: "ok", model_info: { n_ctx: 4096 } });
      }
      if (href.endsWith("/api/tags")) {
        return Response.json({ models: [{ name: "qwen3:8b", size: 123 }] });
      }
      if (href.endsWith("/api/version")) {
        return Response.json({ version: "test-ollama" });
      }
      if (href.endsWith("/api/chat") || href.endsWith("/v1/chat/completions")) {
        const parsedBody = init?.body ? JSON.parse(String(init?.body)) : {};
        return Response.json({
          id: "chatcmpl-test",
          object: "chat.completion",
          created: Date.now(),
          model: parsedBody.model || "qwen3:8b",
          choices: [{
            index: 0,
            message: { role: "assistant", content: "ok" },
            finish_reason: "stop",
          }],
          usage: { prompt_tokens: 1, completion_tokens: 1, total_tokens: 2 },
        });
      }
      return Response.json({}, { status: 404 });
    }) as any;
    ctx = new AppContext({
      ...testConfig,
      database: { ...testConfig.database, path: "./data/test-gateway-proxy.sqlite" },
      security: { ...testConfig.security, requireApiKey: false },
    });
    await ctx.init();
    app = await createProxyApp(ctx);
  });

  afterAll(async () => {
    await app.close();
    await ctx.shutdown();
    globalThis.fetch = originalFetch;
    rmSync("./data/test-gateway-proxy.sqlite", { force: true });
  });

  it("records and releases a GPU job for OpenAI chat completions", async () => {
    const res = await app.inject({
      method: "POST",
      url: "/v1/chat/completions",
      payload: {
        model: "qwen3:8b",
        messages: [{ role: "user", content: "hello" }],
        stream: false,
      },
    });

    expect(res.statusCode).toBe(200);
    expect(res.headers["x-inferdeck-job-id"]).toBeTruthy();
    expect(ctx.queueStore.getLeased()).toHaveLength(0);

    const jobs = ctx.db.client.exec("SELECT status, type, resource_class FROM jobs ORDER BY created_at DESC LIMIT 1");
    expect(jobs[0].values[0]).toEqual(["succeeded", "openai_chat", "gpu_llm"]);
  });
});
