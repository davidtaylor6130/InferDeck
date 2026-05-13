import type { FastifyInstance } from "fastify";
import { assertNotSelfProxy } from "./proxy-utils";
import type { WorkloadLease } from "../../services/WorkloadCoordinator";

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

export function registerProxyOllamaRoutes(app: FastifyInstance): void {
  const getOllamaBaseUrl = () => (app as any).ctx?.()?.ollama?.baseUrl || "http://127.0.0.1:11435";

  // GET /api/tags
  app.get("/api/tags", async (_req, reply) => {
    try {
      const ollamaBaseUrl = getOllamaBaseUrl();
      assertNotSelfProxy(_req, ollamaBaseUrl);
      const res = await fetch(`${ollamaBaseUrl}/api/tags`);
      const text = await res.text();
      reply.headers({ "X-AI-Gateway": "inferdeck" });
      return reply.type("application/json").send(text);
    } catch (err: any) {
      return reply.status(502).send({ error: `Ollama unreachable: ${err.message}` });
    }
  });

  // POST /api/show
  app.post("/api/show", async (req, reply) => {
    try {
      const ollamaBaseUrl = getOllamaBaseUrl();
      assertNotSelfProxy(req, ollamaBaseUrl);
      const res = await fetch(`${ollamaBaseUrl}/api/show`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(req.body),
      });
      const text = await res.text();
      reply.headers({ "X-AI-Gateway": "inferdeck" });
      return reply.type("application/json").send(text);
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  // GET /api/ps
  app.get("/api/ps", async (_req, reply) => {
    try {
      const ollamaBaseUrl = getOllamaBaseUrl();
      assertNotSelfProxy(_req, ollamaBaseUrl);
      const res = await fetch(`${ollamaBaseUrl}/api/ps`);
      const text = await res.text();
      reply.headers({ "X-AI-Gateway": "inferdeck" });
      return reply.type("application/json").send(text);
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  // GET /api/version
  app.get("/api/version", async (_req, reply) => {
    try {
      const ollamaBaseUrl = getOllamaBaseUrl();
      assertNotSelfProxy(_req, ollamaBaseUrl);
      const res = await fetch(`${ollamaBaseUrl}/api/version`);
      const text = await res.text();
      reply.headers({ "X-AI-Gateway": "inferdeck" });
      return reply.type("application/json").send(text);
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  // POST /api/chat (with streaming support)
  app.post("/api/chat", async (req, reply) => {
    const stream = (req.query as any).stream ?? (req.body as any)?.stream ?? false;
    const isStreaming = typeof stream === "string" ? stream === "true" : !!stream;
    const body = req.body as any || {};
    const ctx = (app as any).ctx?.();
    const lease = await ctx.workloads.acquire(req, {
      type: "ollama_chat",
      resourceClass: "gpu_llm",
      priority: 70,
      requestPath: "/api/chat",
      requestMethod: "POST",
      payload: { model: body.model, messages: body.messages?.slice?.(0, 1), stream: isStreaming },
    });
    const release = releaseOnce(lease);
    req.raw.on("close", () => release("cancelled", { reason: "client_closed" }));

    try {
      const ollamaBaseUrl = getOllamaBaseUrl();
      assertNotSelfProxy(req, ollamaBaseUrl);
      const response = await fetch(`${ollamaBaseUrl}/api/chat`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(req.body),
      });
      if (!response.ok) throw new Error(`Ollama returned ${response.status}: ${await response.text()}`);

      if (isStreaming && response.body) {
        reply.headers({
          "Content-Type": "application/x-ndjson",
          "X-AI-Gateway": "inferdeck",
          "X-InferDeck-Job-Id": lease.jobId,
        });
        return reply.send(trackedStream(response.body, release));
      }

      const text = await response.text();
      reply.headers({ "X-AI-Gateway": "inferdeck", "X-InferDeck-Job-Id": lease.jobId });
      release("succeeded", { responseBytes: text.length });
      return reply.type("application/json").send(text);
    } catch (err: any) {
      release("failed", { error: err.message });
      return reply.status(502).send({ error: err.message });
    }
  });

  // POST /api/generate (with streaming support)
  app.post("/api/generate", async (req, reply) => {
    const stream = (req.query as any).stream ?? (req.body as any)?.stream ?? false;
    const isStreaming = typeof stream === "string" ? stream === "true" : !!stream;
    const body = req.body as any || {};
    const ctx = (app as any).ctx?.();
    const lease = await ctx.workloads.acquire(req, {
      type: "ollama_generate",
      resourceClass: "gpu_llm",
      priority: 70,
      requestPath: "/api/generate",
      requestMethod: "POST",
      payload: { model: body.model, prompt: String(body.prompt ?? "").slice(0, 200), stream: isStreaming },
    });
    const release = releaseOnce(lease);
    req.raw.on("close", () => release("cancelled", { reason: "client_closed" }));

    try {
      const ollamaBaseUrl = getOllamaBaseUrl();
      assertNotSelfProxy(req, ollamaBaseUrl);
      const response = await fetch(`${ollamaBaseUrl}/api/generate`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(req.body),
      });
      if (!response.ok) throw new Error(`Ollama returned ${response.status}: ${await response.text()}`);

      if (isStreaming && response.body) {
        reply.headers({
          "Content-Type": "application/x-ndjson",
          "X-AI-Gateway": "inferdeck",
          "X-InferDeck-Job-Id": lease.jobId,
        });
        return reply.send(trackedStream(response.body, release));
      }

      const text = await response.text();
      reply.headers({ "X-AI-Gateway": "inferdeck", "X-InferDeck-Job-Id": lease.jobId });
      release("succeeded", { responseBytes: text.length });
      return reply.type("application/json").send(text);
    } catch (err: any) {
      release("failed", { error: err.message });
      return reply.status(502).send({ error: err.message });
    }
  });

  // POST /api/embed
  app.post("/api/embed", async (req, reply) => {
    const body = req.body as any || {};
    const ctx = (app as any).ctx?.();
    const lease = await ctx.workloads.acquire(req, {
      type: "ollama_embed",
      resourceClass: "gpu_llm",
      priority: 55,
      requestPath: "/api/embed",
      requestMethod: "POST",
      payload: { model: body.model },
    });
    const release = releaseOnce(lease);
    req.raw.on("close", () => release("cancelled", { reason: "client_closed" }));
    try {
      const ollamaBaseUrl = getOllamaBaseUrl();
      assertNotSelfProxy(req, ollamaBaseUrl);
      const res = await fetch(`${ollamaBaseUrl}/api/embed`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(req.body),
      });
      if (!res.ok) throw new Error(`Ollama returned ${res.status}: ${await res.text()}`);
      const text = await res.text();
      reply.headers({ "X-AI-Gateway": "inferdeck", "X-InferDeck-Job-Id": lease.jobId });
      release("succeeded", { responseBytes: text.length });
      return reply.type("application/json").send(text);
    } catch (err: any) {
      release("failed", { error: err.message });
      return reply.status(502).send({ error: err.message });
    }
  });
}
