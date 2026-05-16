import type { FastifyInstance } from "fastify";
import { assertNotSelfProxy } from "./proxy-utils";
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

export function registerProxyLlamaRoutes(app: FastifyInstance): void {
  const getBaseUrl = () => {
    const ctx = getCtx(app);
    return ctx?.backend?.baseUrl || "http://127.0.0.1:11434";
  };

  app.get("/api/tags", async (_req, reply) => {
    try {
      const ctx = getCtx(app);
      assertNotSelfProxy(_req, getBaseUrl());
      const models = ctx?.backend?.listModels() ?? [];
      const backendModels = {
        models: models.map((m) => ({
          name: m.name,
          model: m.name,
          size: m.size,
          digest: "gguf:" + m.path,
          details: {
            parent_model: "",
            format: m.format,
            family: m.name,
            families: [m.name],
            parameter_size: "",
            quantization_level: "",
          },
          modified_at: m.modified_at,
        })),
      };
      reply.headers({ "X-AI-Gateway": "inferdeck" });
      return reply.send(backendModels);
    } catch (err: any) {
      return reply.status(502).send({ error: `llama.cpp unreachable: ${err.message}` });
    }
  });

  app.post("/api/show", async (req, reply) => {
    try {
      const ctx = getCtx(app);
      const body = req.body as { name?: string; model?: string };
      const name = body?.name ?? body?.model ?? "";
      const models = ctx?.backend?.listModels() ?? [];
      const found = models.find((m) => m.name === name || m.path.endsWith(name));
      if (!found) return reply.status(404).send({ error: `model "${name}" not found` });
      reply.headers({ "X-AI-Gateway": "inferdeck" });
      return reply.send({
        name: found.name,
        model: found.name,
        size: found.size,
        digest: "gguf:" + found.path,
        details: {
          parent_model: "",
          format: found.format,
          family: found.name,
          families: [found.name],
          parameter_size: "",
          quantization_level: "",
        },
        modified_at: found.modified_at,
      });
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  app.get("/api/ps", async (_req, reply) => {
    try {
      const ctx = getCtx(app);
      assertNotSelfProxy(_req, getBaseUrl());
      const snap = ctx?.backend?.getSnapshot();
      const models = snap?.model ? [{
        name: snap.model,
        model: snap.model,
        size: 0,
        digest: "",
        details: { parent_model: "", format: "gguf", family: "", families: [], parameter_size: "", quantization_level: "" },
        size_vram: 0,
        expires_at: "",
        ttl: -1,
      }] : [];
      reply.headers({ "X-AI-Gateway": "inferdeck" });
      return reply.send({ models });
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  app.get("/api/version", async (_req, reply) => {
    try {
      const baseUrl = getBaseUrl();
      assertNotSelfProxy(_req, baseUrl);
      const res = await fetch(`${baseUrl}/health`);
      if (!res.ok) throw new Error(`llama.cpp returned ${res.status}`);
      reply.headers({ "X-AI-Gateway": "inferdeck" });
      return reply.send({ version: "llama.cpp", status: "ok" });
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  app.post("/api/chat", async (req, reply) => {
    const stream = (req.query as any).stream ?? (req.body as any)?.stream ?? false;
    const isStreaming = typeof stream === "string" ? stream === "true" : !!stream;
    const body = req.body as any || {};
    const ctx = getCtx(app);
    const lease = await ctx.workloads.acquire(req, {
      type: "llama_chat",
      resourceClass: "gpu_llm",
      priority: 70,
      requestPath: "/api/chat",
      requestMethod: "POST",
      payload: { model: body.model, messages: body.messages?.slice?.(0, 1), stream: isStreaming },
    });
    const release = releaseOnce(lease);
    req.raw.on("close", () => release("cancelled", { reason: "client_closed" }));

    try {
      const baseUrl = getBaseUrl();
      assertNotSelfProxy(req, baseUrl);
      const openaiBody: Record<string, unknown> = {
        model: body.model,
        messages: body.messages ?? [],
        stream: isStreaming,
      };
      if (body.options?.temperature) openaiBody.temperature = body.options.temperature;
      if (body.options?.top_p) openaiBody.top_p = body.options.top_p;
      if (body.options?.max_tokens) openaiBody.max_tokens = body.options.max_tokens;
      if (body.options?.stop) openaiBody.stop = body.options.stop;

      const response = await fetch(`${baseUrl}/v1/chat/completions`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(openaiBody),
      });
      if (!response.ok) throw new Error(`llama.cpp returned ${response.status}: ${await response.text()}`);

      if (isStreaming && response.body) {
        reply.headers({
          "Content-Type": "text/event-stream",
          "Cache-Control": "no-cache",
          "Connection": "keep-alive",
          "X-AI-Gateway": "inferdeck",
          "X-InferDeck-Job-Id": lease.jobId,
        });
        return reply.send(trackedStream(response.body, release));
      }

      const json = await response.json();
      reply.headers({ "X-AI-Gateway": "inferdeck", "X-InferDeck-Job-Id": lease.jobId });
      release("succeeded", { responseBytes: JSON.stringify(json).length });
      return reply.send(json);
    } catch (err: any) {
      release("failed", { error: err.message });
      return reply.status(502).send({ error: err.message });
    }
  });

  app.post("/api/generate", async (req, reply) => {
    const stream = (req.query as any).stream ?? (req.body as any)?.stream ?? false;
    const isStreaming = typeof stream === "string" ? stream === "true" : !!stream;
    const body = req.body as any || {};
    const ctx = getCtx(app);
    const lease = await ctx.workloads.acquire(req, {
      type: "llama_generate",
      resourceClass: "gpu_llm",
      priority: 70,
      requestPath: "/api/generate",
      requestMethod: "POST",
      payload: { model: body.model, prompt: String(body.prompt ?? "").slice(0, 200), stream: isStreaming },
    });
    const release = releaseOnce(lease);
    req.raw.on("close", () => release("cancelled", { reason: "client_closed" }));

    try {
      const baseUrl = getBaseUrl();
      assertNotSelfProxy(req, baseUrl);
      const openaiBody: Record<string, unknown> = {
        model: body.model,
        prompt: body.prompt ?? "",
        stream: isStreaming,
      };
      if (body.options?.temperature) openaiBody.temperature = body.options.temperature;
      if (body.options?.top_p) openaiBody.top_p = body.options.top_p;
      if (body.options?.max_tokens) openaiBody.max_tokens = body.options.max_tokens;

      const response = await fetch(`${baseUrl}/v1/completions`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(openaiBody),
      });
      if (!response.ok) throw new Error(`llama.cpp returned ${response.status}: ${await response.text()}`);

      if (isStreaming && response.body) {
        reply.headers({
          "Content-Type": "text/event-stream",
          "Cache-Control": "no-cache",
          "Connection": "keep-alive",
          "X-AI-Gateway": "inferdeck",
          "X-InferDeck-Job-Id": lease.jobId,
        });
        return reply.send(trackedStream(response.body, release));
      }

      const json = await response.json();
      reply.headers({ "X-AI-Gateway": "inferdeck", "X-InferDeck-Job-Id": lease.jobId });
      release("succeeded", { responseBytes: JSON.stringify(json).length });
      return reply.send(json);
    } catch (err: any) {
      release("failed", { error: err.message });
      return reply.status(502).send({ error: err.message });
    }
  });

  app.post("/api/embed", async (req, reply) => {
    const body = req.body as any || {};
    const ctx = getCtx(app);
    const lease = await ctx.workloads.acquire(req, {
      type: "llama_embed",
      resourceClass: "gpu_llm",
      priority: 55,
      requestPath: "/api/embed",
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
        body: JSON.stringify({ model: body.model, input: body.input ?? body.prompt ?? "" }),
      });
      if (!res.ok) throw new Error(`llama.cpp returned ${res.status}: ${await res.text()}`);
      const json = await res.json();
      reply.headers({ "X-AI-Gateway": "inferdeck", "X-InferDeck-Job-Id": lease.jobId });
      release("succeeded", { responseBytes: JSON.stringify(json).length });
      return reply.send(json);
    } catch (err: any) {
      release("failed", { error: err.message });
      return reply.status(502).send({ error: err.message });
    }
  });
}
