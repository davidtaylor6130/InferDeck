import { describe, it, expect, beforeAll, afterAll, vi } from "vitest";
import type { FastifyInstance } from "fastify";
import { rmSync } from "node:fs";
import { AppContext } from "../../apps/gateway-service/src/app";
import { createProxyApp } from "../../apps/gateway-service/src/http/server";
import { testConfig } from "../fixtures/testConfig";

describe("OpenCode API compatibility", () => {
  let app: FastifyInstance;
  let ctx: AppContext;
  const originalFetch = globalThis.fetch;

  const longSystemPrompt = `You are an expert software engineer. You have access to the following tools:

1. read - Read file contents from the filesystem
2. write - Write content to files
3. edit - Edit existing files
4. bash - Execute shell commands
5. glob - Search for files by pattern
6. grep - Search for content within files
7. webSearch - Search the web for information
8. webFetch - Fetch content from URLs

For each task, analyze the request carefully, plan your approach, then use the appropriate tools.
Always verify your changes before completing.`.repeat(20);

  beforeAll(async () => {
    globalThis.fetch = vi.fn(async (url: string | URL, init?: RequestInit) => {
      const href = String(url);
      if (href.endsWith("/v1/models")) {
        return Response.json({
          object: "list",
          data: [
            { id: "qwen3-coder-30b-a3b-instruct:latest", object: "model", created: 1700000000, owned_by: "community" },
          ],
        });
      }
      if (href.endsWith("/v1/chat/completions")) {
        const parsedBody = init?.body ? JSON.parse(String(init?.body)) : {};
        const isStream = parsedBody.stream ?? false;

        if (isStream) {
          const encoder = new TextEncoder();
          const chunks = [
            `data: {"id":"chatcmpl-test","object":"chat.completion.chunk","created":${Date.now()},"model":"${parsedBody.model || "qwen3:8b"}","choices":[{"index":0,"delta":{"role":"assistant","content":"Hello"},"finish_reason":null}]}\n\n`,
            `data: {"id":"chatcmpl-test","object":"chat.completion.chunk","created":${Date.now()},"model":"${parsedBody.model || "qwen3:8b"}","choices":[{"index":0,"delta":{"content":"! I can"},"finish_reason":null}]}\n\n`,
            `data: {"id":"chatcmpl-test","object":"chat.completion.chunk","created":${Date.now()},"model":"${parsedBody.model || "qwen3:8b"}","choices":[{"index":0,"delta":{"content":" help you with that."},"finish_reason":null}]}\n\n`,
            `data: {"id":"chatcmpl-test","object":"chat.completion.chunk","created":${Date.now()},"model":"${parsedBody.model || "qwen3:8b"}","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}\n\n`,
            "data: [DONE]\n\n",
          ];
          const stream = new ReadableStream({
            start(controller) {
              for (const chunk of chunks) {
                controller.enqueue(encoder.encode(chunk));
              }
              controller.close();
            },
          });
          return new Response(stream, {
            headers: { "Content-Type": "text/event-stream" },
          });
        }

        return Response.json({
          id: "chatcmpl-test",
          object: "chat.completion",
          created: Date.now(),
          model: parsedBody.model || "qwen3-coder-30b-a3b-instruct:latest",
          choices: [{
            index: 0,
            message: {
              role: "assistant",
              content: "I'll help you with that task. Let me start by reading the relevant files.",
              tool_calls: parsedBody.messages?.some((m: any) => m.role === "user" && m.content?.includes("tool"))
                ? [{
                    id: "call_abc123",
                    type: "function",
                    function: { name: "read", arguments: '{"filePath":"src/main.ts"}' },
                  }]
                : undefined,
            },
            finish_reason: parsedBody.messages?.some((m: any) => m.role === "user" && m.content?.includes("tool")) ? "tool_calls" : "stop",
          }],
          usage: { prompt_tokens: 8500, completion_tokens: 45, total_tokens: 8545 },
        });
      }
      if (href.endsWith("/v1/completions")) {
        return Response.json({
          id: "cmpl-test",
          object: "text_completion",
          created: Date.now(),
          model: "qwen3-coder-30b-a3b-instruct:latest",
          choices: [{ index: 0, text: "Hello! I can help you with that.", finish_reason: "stop" }],
          usage: { prompt_tokens: 10, completion_tokens: 8, total_tokens: 18 },
        });
      }
      if (href.endsWith("/health")) {
        return Response.json({ status: "ok", model_info: { n_ctx: 100000 } });
      }
      return Response.json({}, { status: 404 });
    }) as any;

    ctx = new AppContext({
      ...testConfig,
      database: { ...testConfig.database, path: "./data/test-opencode.sqlite" },
      security: { ...testConfig.security, requireApiKey: false },
    });
    await ctx.init();
    app = await createProxyApp(ctx);
  });

  afterAll(async () => {
    await app.close();
    await ctx.shutdown();
    globalThis.fetch = originalFetch;
    rmSync("./data/test-opencode.sqlite", { force: true });
  });

  it("should list models in OpenAI format for OpenCode model picker", async () => {
    const res = await app.inject({
      method: "GET",
      url: "/v1/models",
    });
    expect(res.statusCode).toBe(200);
    const body = JSON.parse(res.body);
    expect(body.object).toBe("list");
    expect(body.data).toBeInstanceOf(Array);
    expect(body.data.length).toBeGreaterThan(0);
    expect(body.data[0]).toHaveProperty("id");
    expect(body.data[0]).toHaveProperty("object", "model");
  });

  it("should handle OpenCode-style chat completion with long system prompt", async () => {
    const res = await app.inject({
      method: "POST",
      url: "/v1/chat/completions",
      payload: {
        model: "qwen3-coder-30b-a3b-instruct:latest",
        messages: [
          { role: "system", content: longSystemPrompt },
          { role: "user", content: "Review the code in src/main.ts and suggest improvements." },
        ],
        stream: false,
      },
    });
    expect(res.statusCode).toBe(200);
    const body = JSON.parse(res.body);
    expect(body.choices[0].message.role).toBe("assistant");
    expect(body.choices[0].message.content).toBeTruthy();
    expect(body.usage.prompt_tokens).toBeGreaterThan(8000);
    expect(res.headers["x-inferdeck-job-id"]).toBeTruthy();
  });

  it("should handle streaming chat completions (OpenCode SSE mode)", async () => {
    const res = await app.inject({
      method: "POST",
      url: "/v1/chat/completions",
      payload: {
        model: "qwen3-coder-30b-a3b-instruct:latest",
        messages: [
          { role: "system", content: "You are a helpful assistant." },
          { role: "user", content: "Write a function to calculate fibonacci numbers." },
        ],
        stream: true,
      },
    });
    expect(res.statusCode).toBe(200);
    expect(res.headers["content-type"]).toContain("text/event-stream");
    expect(res.headers["x-inferdeck-job-id"]).toBeTruthy();
    const body = res.body;
    expect(body).toContain("data: ");
    expect(body).toContain("[DONE]");
    expect(body).toContain("chatcmpl-test");
  });

  it("should handle tool call responses (OpenCode function calling)", async () => {
    const res = await app.inject({
      method: "POST",
      url: "/v1/chat/completions",
      payload: {
        model: "qwen3-coder-30b-a3b-instruct:latest",
        messages: [
          { role: "system", content: "You are an expert coder with access to tools." },
          { role: "user", content: "Use the read tool to check the codebase structure." },
        ],
        stream: false,
      },
    });
    expect(res.statusCode).toBe(200);
    const body = JSON.parse(res.body);
    expect(body.choices[0].finish_reason).toBe("tool_calls");
    expect(body.choices[0].message.tool_calls).toBeInstanceOf(Array);
    if (body.choices[0].message.tool_calls) {
      const toolCall = body.choices[0].message.tool_calls[0];
      expect(toolCall.type).toBe("function");
      expect(toolCall.function.name).toBe("read");
    }
  });

  it("should handle model name with OpenCode's model ID format", async () => {
    const res = await app.inject({
      method: "POST",
      url: "/v1/chat/completions",
      payload: {
        model: "qwen3-coder-30b-a3b-instruct:latest",
        messages: [
          { role: "user", content: "hello" },
        ],
        stream: false,
      },
    });
    expect(res.statusCode).toBe(200);
    const body = JSON.parse(res.body);
    expect(body.model).toContain("qwen3-coder-30b-a3b-instruct");
  });

  it("should handle concurrent requests (OpenCode + other clients simultaneously)", async () => {
    const results = await Promise.all([
      app.inject({
        method: "POST",
        url: "/v1/chat/completions",
        payload: {
          model: "qwen3-coder-30b-a3b-instruct:latest",
          messages: [{ role: "user", content: "request A" }],
          stream: false,
        },
      }),
      app.inject({
        method: "POST",
        url: "/v1/chat/completions",
        payload: {
          model: "qwen3-coder-30b-a3b-instruct:latest",
          messages: [{ role: "user", content: "request B" }],
          stream: false,
        },
      }),
      app.inject({
        method: "POST",
        url: "/v1/chat/completions",
        payload: {
          model: "qwen3-coder-30b-a3b-instruct:latest",
          messages: [{ role: "user", content: "request C" }],
          stream: false,
        },
      }),
    ]);
    for (const res of results) {
      expect(res.statusCode).toBe(200);
      const body = JSON.parse(res.body);
      expect(body.choices[0].message.content).toBeTruthy();
      expect(res.headers["x-inferdeck-job-id"]).toBeTruthy();
    }
    expect(ctx.queueStore.getLeased()).toHaveLength(0);
  });

  it("should reject request with body over 50MB", async () => {
    const hugeContent = "x".repeat(52 * 1024 * 1024);
    const res = await app.inject({
      method: "POST",
      url: "/v1/chat/completions",
      payload: {
        model: "qwen3-coder-30b-a3b-instruct:latest",
        messages: [{ role: "user", content: hugeContent }],
      },
    });
    expect(res.statusCode).toBe(413);
  });
});
