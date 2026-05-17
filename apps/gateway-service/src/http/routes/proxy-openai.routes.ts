import type { FastifyInstance } from "fastify";
import { assertNotSelfProxy, ensureModel } from "./proxy-utils";
import type { WorkloadLease } from "../../services/WorkloadCoordinator";
import type { AppContext } from "../../app";

function releaseOnce(lease: WorkloadLease): (status: "succeeded" | "failed" | "cancelled", data?: Record<string, unknown>) => void {
  let released = false;
  return (status, data = {}) => {
    if (released) return;
    released = true;
    lease.release(status, data);
  };
}

function trackedStream(source: ReadableStream<Uint8Array>, release: ReturnType<typeof releaseOnce>): ReadableStream<Uint8Array> {
  return source.pipeThrough(new TransformStream<Uint8Array, Uint8Array>({
    transform(chunk, controller) {
      controller.enqueue(chunk);
    },
    flush() {
      release("succeeded", { streamed: true });
    },
  }));
}

function getCtx(app: FastifyInstance): AppContext {
  return (app as any).ctx?.();
}

export function registerProxyOpenAIRoutes(app: FastifyInstance): void {
  const getBaseUrl = () => {
    const ctx = getCtx(app);
    return ctx?.backend?.baseUrl || "http://127.0.0.1:11434";
  };

  app.get("/v1/models", async (_req, reply) => {
    try {
      const baseUrl = getBaseUrl();
      assertNotSelfProxy(_req, baseUrl);
      const res = await fetch(`${baseUrl}/v1/models`);
      if (!res.ok) throw new Error(`llama.cpp returned ${res.status}`);
      const data = await res.json();
      return reply.send(data);
    } catch {
      const ctx = getCtx(app);
      const models = ctx?.backend?.listModels() ?? [];
      return reply.send({
        object: "list",
        data: models.map((m) => ({
          id: m.name,
          object: "model",
          created: Math.floor(new Date(m.modified_at).getTime() / 1000),
          owned_by: "community",
        })),
      });
    }
  });

  app.post("/v1/chat/completions", async (req, reply) => {
    const body = req.body as any || {};
    const stream = body?.stream ?? false;
    const startTime = Date.now();
    const ctx = getCtx(app);

    try {
      await ensureModel(body.model, ctx);
    } catch (err: any) {
      return reply.status(502).send({ error: { message: err.message, type: "model_load_error" } });
    }

    const lease = await ctx.workloads.acquire(req, {
      type: "openai_chat",
      resourceClass: "gpu_llm",
      priority: 70,
      requestPath: "/v1/chat/completions",
      requestMethod: "POST",
      payload: { model: body.model, messages: body?.messages?.slice(0, 1), stream },
    });
    const release = releaseOnce(lease);
    req.raw.on("close", () => release("cancelled", { reason: "client_closed" }));

    try {
      const baseUrl = getBaseUrl();
      assertNotSelfProxy(req, baseUrl);
      const response = await fetch(`${baseUrl}/v1/chat/completions`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          ...(body?.api_key ? { Authorization: `Bearer ${body.api_key}` } : {}),
        },
        body: JSON.stringify(body),
      });
      if (!response.ok) throw new Error(`llama.cpp returned ${response.status}: ${await response.text()}`);

      if (stream && response.body) {
        reply.headers({
          "Content-Type": "text/event-stream",
          "Cache-Control": "no-cache",
          "Connection": "keep-alive",
          "X-AI-Gateway": "inferdeck",
          "X-AI-Resource-Class": "gpu_llm",
          "X-InferDeck-Job-Id": lease.jobId,
        });
        return reply.send(trackedStream(response.body, release));
      }

      const json = await response.json();
      reply.headers({ "X-AI-Gateway": "inferdeck", "X-AI-Resource-Class": "gpu_llm" });
      reply.header("X-InferDeck-Job-Id", lease.jobId);
      release("succeeded", { response: json, durationMs: Date.now() - startTime });
      return reply.send(json);
    } catch (err: any) {
      release("failed", { error: err.message, durationMs: Date.now() - startTime });
      return reply.status(502).send({ error: { message: err.message, type: "proxy_error" } });
    }
  });

  app.post("/v1/completions", async (req, reply) => {
    const body = req.body as any;
    const stream = body?.stream ?? false;
    const ctx = getCtx(app);
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

    try {
      const baseUrl = getBaseUrl();
      assertNotSelfProxy(req, baseUrl);
      const response = await fetch(`${baseUrl}/v1/completions`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (!response.ok) throw new Error(`llama.cpp returned ${response.status}: ${await response.text()}`);

      if (stream && response.body) {
        reply.headers({
          "Content-Type": "text/event-stream",
          "Cache-Control": "no-cache",
          "Connection": "keep-alive",
          "X-AI-Gateway": "inferdeck",
          "X-AI-Resource-Class": "gpu_llm",
          "X-InferDeck-Job-Id": lease.jobId,
        });
        return reply.send(trackedStream(response.body, release));
      }

      const json = await response.json();
      reply.headers({ "X-AI-Gateway": "inferdeck", "X-AI-Resource-Class": "gpu_llm" });
      reply.header("X-InferDeck-Job-Id", lease.jobId);
      release("succeeded", { response: json });
      return reply.send(json);
    } catch (err: any) {
      release("failed", { error: err.message });
      return reply.status(502).send({ error: { message: err.message, type: "proxy_error" } });
    }
  });

  app.post("/v1/embeddings", async (req, reply) => {
    const body = req.body as any;
    const ctx = getCtx(app);
    const lease = await ctx.workloads.acquire(req, {
      type: "openai_embedding",
      resourceClass: "gpu_llm",
      priority: 55,
      requestPath: "/v1/embeddings",
      requestMethod: "POST",
      payload: { model: body.model },
    });
    const release = releaseOnce(lease);
    req.raw.on("close", () => release("cancelled", { reason: "client_closed" }));

    try {
      const baseUrl = getBaseUrl();
      assertNotSelfProxy(req, baseUrl);
      const res = await fetch(`${baseUrl}/v1/embeddings`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (!res.ok) throw new Error(`llama.cpp returned ${res.status}: ${await res.text()}`);
      const json = await res.json();
      reply.header("X-InferDeck-Job-Id", lease.jobId);
      release("succeeded", { resultCount: json.data?.length ?? 0 });
      return reply.send(json);
    } catch (err: any) {
      release("failed", { error: err.message });
      return reply.status(502).send({ error: { message: err.message, type: "proxy_error" } });
    }
  });
}
