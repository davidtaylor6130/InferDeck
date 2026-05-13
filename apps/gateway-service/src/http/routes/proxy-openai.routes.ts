import type { FastifyInstance } from "fastify";
import { assertNotSelfProxy } from "./proxy-utils";
import type { WorkloadLease } from "../../services/WorkloadCoordinator";

function createOllamaToOpenAIStream(
  source: ReadableStream<Uint8Array>,
  model: string,
  mapper: (data: any) => any,
  onDone?: (status: "succeeded" | "failed", data: Record<string, unknown>) => void
): ReadableStream<Uint8Array> {
  const decoder = new TextDecoder();
  const encoder = new TextEncoder();
  let buffer = "";

  return source.pipeThrough(new TransformStream<Uint8Array, Uint8Array>({
    transform(chunk, controller) {
      buffer += decoder.decode(chunk, { stream: true });
      const lines = buffer.split("\n");
      buffer = lines.pop() ?? "";

      try {
        for (const line of lines) {
          const trimmed = line.trim();
          if (!trimmed) continue;
          const data = JSON.parse(trimmed);
          if (data.done) continue;
          controller.enqueue(encoder.encode(`data: ${JSON.stringify(mapper({ ...data, model }))}\n\n`));
        }
      } catch (err) {
        onDone?.("failed", { error: err instanceof Error ? err.message : String(err) });
        throw err;
      }
    },
    flush(controller) {
      try {
        if (buffer.trim()) {
          const data = JSON.parse(buffer.trim());
          if (!data.done) {
            controller.enqueue(encoder.encode(`data: ${JSON.stringify(mapper({ ...data, model }))}\n\n`));
          }
        }
        controller.enqueue(encoder.encode("data: [DONE]\n\n"));
        onDone?.("succeeded", { streamed: true });
      } catch (err) {
        onDone?.("failed", { error: err instanceof Error ? err.message : String(err) });
        throw err;
      }
    },
  }));
}

function releaseOnce(lease: WorkloadLease): (status: "succeeded" | "failed" | "cancelled", data?: Record<string, unknown>) => void {
  let released = false;
  return (status, data = {}) => {
    if (released) return;
    released = true;
    lease.release(status, data);
  };
}

export function registerProxyOpenAIRoutes(app: FastifyInstance): void {
  const getOllamaBaseUrl = () => (app as any).ctx?.()?.ollama?.baseUrl || "http://127.0.0.1:11435";

  // GET /v1/models
  app.get("/v1/models", async (_req, reply) => {
    try {
      const ollamaBaseUrl = getOllamaBaseUrl();
      assertNotSelfProxy(_req, ollamaBaseUrl);
      const res = await fetch(`${ollamaBaseUrl}/api/tags`);
      const data = await res.json();
      const models = (data.models ?? []).map((m: any) => ({
        id: m.name,
        object: "model",
        created: Date.now(),
        owned_by: "community",
      }));
      return reply.send({ object: "list", data: models });
    } catch {
      return reply.send({ object: "list", data: [] });
    }
  });

  // POST /v1/chat/completions
  app.post("/v1/chat/completions", async (req, reply) => {
    const body = req.body as any || {};
    const stream = body?.stream ?? false;
    const modelName = body?.model;
    const startTime = Date.now();
    const ctx = (app as any).ctx?.();
    const lease = await ctx.workloads.acquire(req, {
      type: "openai_chat",
      resourceClass: "gpu_llm",
      priority: 70,
      requestPath: "/v1/chat/completions",
      requestMethod: "POST",
      payload: { model: modelName, messages: body?.messages?.slice(0, 1), stream },
    });
    const release = releaseOnce(lease);
    req.raw.on("close", () => release("cancelled", { reason: "client_closed" }));

    // Convert OpenAI format to Ollama format
    const ollamaBody: any = {
      model: modelName,
      messages: body?.messages ?? [],
      stream: stream,
      options: body?.options ?? {},
    };

    try {
      const ollamaBaseUrl = getOllamaBaseUrl();
      assertNotSelfProxy(req, ollamaBaseUrl);
      const response = await fetch(`${ollamaBaseUrl}/api/chat`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${body?.api_key ?? "x"}`,
        },
        body: JSON.stringify(ollamaBody),
      });
      if (!response.ok) throw new Error(`Ollama returned ${response.status}: ${await response.text()}`);

      if (stream && response.body) {
        reply.headers({
          "Content-Type": "text/event-stream",
          "Cache-Control": "no-cache",
          "Connection": "keep-alive",
          "X-AI-Gateway": "inferdeck",
          "X-AI-Resource-Class": "gpu_llm",
          "X-InferDeck-Job-Id": lease.jobId,
        });
        return reply.send(createOllamaToOpenAIStream(response.body, body.model, (data) => ({
          id: "chatcmpl-gateway",
          object: "chat.completion.chunk",
          created: Math.floor(Date.now() / 1000),
          model: data.model,
          choices: [
            {
              index: 0,
              delta: { content: data.message?.content ?? "" },
              finish_reason: null,
            },
          ],
        }), (status, data) => release(status, { ...data, durationMs: Date.now() - startTime })));
      }

      // Non-streaming
      const text = await response.text();
      const data = JSON.parse(text);
      const openaiResponse = {
        id: "chatcmpl-gateway",
        object: "chat.completion",
        created: Math.floor(Date.now() / 1000),
        model: body.model,
        choices: [
          {
            index: 0,
            message: { role: "assistant", content: data.message?.content ?? data.response ?? "" },
            finish_reason: data.done ? "stop" : null,
          },
        ],
        usage: {
          prompt_tokens: data.prompt_eval_count ?? 0,
          completion_tokens: data.eval_count ?? 0,
          total_tokens: (data.prompt_eval_count ?? 0) + (data.eval_count ?? 0),
        },
      };
      reply.headers({ "X-AI-Gateway": "inferdeck", "X-AI-Resource-Class": "gpu_llm" });
      reply.header("X-InferDeck-Job-Id", lease.jobId);
      release("succeeded", { response: data, durationMs: Date.now() - startTime });
      return reply.send(openaiResponse);
    } catch (err: any) {
      release("failed", { error: err.message, durationMs: Date.now() - startTime });
      return reply.status(502).send({ error: { message: err.message, type: "proxy_error" } });
    }
  });

  // POST /v1/completions
  app.post("/v1/completions", async (req, reply) => {
    const body = req.body as any;
    const stream = body.stream ?? false;
    const ctx = (app as any).ctx?.();
    const lease = await ctx.workloads.acquire(req, {
      type: "openai_completion",
      resourceClass: "gpu_llm",
      priority: 70,
      requestPath: "/v1/completions",
      requestMethod: "POST",
      payload: { model: body.model, prompt: String(body.prompt ?? "").slice(0, 200), stream },
    });
    const release = releaseOnce(lease);
    req.raw.on("close", () => release("cancelled", { reason: "client_closed" }));

    const ollamaBody: any = {
      model: body.model,
      prompt: body.prompt,
      stream: stream,
      options: body.options,
    };

    try {
      const ollamaBaseUrl = getOllamaBaseUrl();
      assertNotSelfProxy(req, ollamaBaseUrl);
      const response = await fetch(`${ollamaBaseUrl}/api/generate`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(ollamaBody),
      });
      if (!response.ok) throw new Error(`Ollama returned ${response.status}: ${await response.text()}`);

      if (stream && response.body) {
        reply.headers({
          "Content-Type": "text/event-stream",
          "Cache-Control": "no-cache",
          "Connection": "keep-alive",
          "X-AI-Gateway": "inferdeck",
          "X-AI-Resource-Class": "gpu_llm",
          "X-InferDeck-Job-Id": lease.jobId,
        });
        return reply.send(createOllamaToOpenAIStream(response.body, body.model, (data) => ({
          id: "cmpl-gateway",
          object: "text_completion",
          created: Math.floor(Date.now() / 1000),
          model: data.model,
          choices: [{ index: 0, text: data.response ?? "", finish_reason: null }],
        }), (status, data) => release(status, data)));
      }

      const text = await response.text();
      const data = JSON.parse(text);
      reply.headers({ "X-AI-Gateway": "inferdeck", "X-AI-Resource-Class": "gpu_llm" });
      reply.header("X-InferDeck-Job-Id", lease.jobId);
      release("succeeded", { response: data });
      return reply.send({
        id: "cmpl-gateway",
        object: "text_completion",
        created: Math.floor(Date.now() / 1000),
        model: body.model,
        choices: [{ index: 0, text: data.response, finish_reason: "stop" }],
      });
    } catch (err: any) {
      release("failed", { error: err.message });
      return reply.status(502).send({ error: { message: err.message, type: "proxy_error" } });
    }
  });

  // POST /v1/embeddings
  app.post("/v1/embeddings", async (req, reply) => {
    const body = req.body as any;
    const input = Array.isArray(body.input) ? body.input : [body.input];
    const ctx = (app as any).ctx?.();
    const lease = await ctx.workloads.acquire(req, {
      type: "openai_embedding",
      resourceClass: "gpu_llm",
      priority: 55,
      requestPath: "/v1/embeddings",
      requestMethod: "POST",
      payload: { model: body.model, inputCount: input.length },
    });
    const release = releaseOnce(lease);
    req.raw.on("close", () => release("cancelled", { reason: "client_closed" }));

    try {
      const results: any[] = [];
      for (const inp of input) {
        const ollamaBaseUrl = getOllamaBaseUrl();
        assertNotSelfProxy(req, ollamaBaseUrl);
        const res = await fetch(`${ollamaBaseUrl}/api/embed`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: body.model, input: inp }),
        });
        const data = await res.json();
        results.push({ embedding: data.embedding, index: results.length });
      }
      const responseBody = {
        object: "list",
        data: results,
        model: body.model,
        usage: { prompt_tokens: 0, total_tokens: 0 },
      };
      reply.header("X-InferDeck-Job-Id", lease.jobId);
      release("succeeded", { resultCount: results.length });
      return reply.send(responseBody);
    } catch (err: any) {
      release("failed", { error: err.message });
      return reply.status(502).send({ error: { message: err.message, type: "proxy_error" } });
    }
  });
}
