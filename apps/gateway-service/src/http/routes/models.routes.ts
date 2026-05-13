import type { FastifyInstance } from "fastify";

export function registerModelsRoutes(app: FastifyInstance): void {
  const getOllamaBaseUrl = () => (app as any).ctx().ollama.baseUrl;

  app.get("/models", async (_req, reply) => {
    const ollamaBaseUrl = getOllamaBaseUrl();
    try {
      const res = await fetch(`${ollamaBaseUrl}/api/tags`);
      if (!res.ok) throw new Error(`Ollama returned ${res.status}`);
      const data = await res.json();
      return reply.send({ models: data.models ?? [], backends: { ollama: ollamaBaseUrl, connected: true } });
    } catch (err) {
      return reply.send({
        models: [],
        backends: {
          ollama: ollamaBaseUrl,
          connected: false,
          error: err instanceof Error ? err.message : String(err),
        },
      });
    }
  });

  app.get("/models/running", async (_req, reply) => {
    const ollamaBaseUrl = getOllamaBaseUrl();
    try {
      const res = await fetch(`${ollamaBaseUrl}/api/ps`);
      if (!res.ok) throw new Error(`Ollama returned ${res.status}`);
      const data = await res.json();
      return reply.send({ running: data.models ?? [], connected: true });
    } catch (err) {
      return reply.send({
        running: [],
        connected: false,
        error: err instanceof Error ? err.message : String(err),
      });
    }
  });

  app.post("/models/pull", async (req, reply) => {
    const ollamaBaseUrl = getOllamaBaseUrl();
    const body = req.body as { name: string };
    try {
      const res = await fetch(`${ollamaBaseUrl}/api/pull`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name: body.name, stream: false }),
      });
      const data = await res.json();
      (app as any).ctx().events.emit("model:changed", { action: "pull", model: body.name });
      return reply.send({ pulled: body.name, status: data.status });
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  app.post("/models/load", async (req, reply) => {
    const ollamaBaseUrl = getOllamaBaseUrl();
    const body = req.body as { model: string; keep_alive?: string };
    try {
      const res = await fetch(`${ollamaBaseUrl}/api/generate`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ model: body.model, prompt: "", stream: false, keep_alive: body.keep_alive ?? "5m" }),
      });
      if (!res.ok) throw new Error(`Ollama returned ${res.status}: ${await res.text()}`);
      const data = await res.json();
      (app as any).ctx().events.emit("model:changed", { action: "load", model: body.model });
      return reply.send({ loaded: body.model, status: data.status ?? "loaded" });
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  app.post("/models/unload", async (req, reply) => {
    const ollamaBaseUrl = getOllamaBaseUrl();
    const body = req.body as { model: string };
    try {
      const res = await fetch(`${ollamaBaseUrl}/api/generate`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ model: body.model, prompt: "", stream: false, keep_alive: 0 }),
      });
      if (!res.ok) throw new Error(`Ollama returned ${res.status}: ${await res.text()}`);
      (app as any).ctx().events.emit("model:changed", { action: "unload", model: body.model });
      return reply.send({ unloaded: body.model });
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  app.delete("/models/:name", async (req, reply) => {
    const ollamaBaseUrl = getOllamaBaseUrl();
    const name = (req.params as { name: string }).name;
    try {
      const res = await fetch(`${ollamaBaseUrl}/api/delete`, {
        method: "DELETE",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ model: name }),
      });
      if (!res.ok) throw new Error(`Ollama returned ${res.status}: ${await res.text()}`);
      (app as any).ctx().events.emit("model:changed", { action: "delete", model: name });
      return reply.send({ deleted: name });
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });
}
