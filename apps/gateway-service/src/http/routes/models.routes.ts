import type { FastifyInstance } from "fastify";
import { createWriteStream, unlinkSync } from "node:fs";
import { join, isAbsolute } from "node:path";
import type { AppContext } from "../../app";
import { normalizeModelName, stripConnectionPrefix } from "../../services/LlamaServerProcessManager";

function getCtx(app: FastifyInstance): AppContext {
  return (app as any).ctx?.();
}

export function registerModelsRoutes(app: FastifyInstance): void {
  app.get("/models", async (_req, reply) => {
    const ctx = getCtx(app);
    try {
      const models = ctx.backend.listModels();
      return reply.send({
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
        backends: { llama_cpp: ctx.backend.baseUrl, connected: ctx.backend.getSnapshot().status === "running" },
      });
    } catch (err) {
      return reply.send({
        models: [],
        backends: {
          llama_cpp: ctx.backend.baseUrl,
          connected: false,
          error: err instanceof Error ? err.message : String(err),
        },
      });
    }
  });

  app.get("/models/running", async (_req, reply) => {
    const ctx = getCtx(app);
    try {
      const snap = ctx.backend.getSnapshot();
      const running = snap.model ? [{
        name: snap.model,
        model: snap.model,
        size: 0,
        digest: "",
        details: { parent_model: "", format: "gguf", family: "", families: [], parameter_size: "", quantization_level: "" },
        size_vram: 0,
        expires_at: "",
        ttl: -1,
      }] : [];
      return reply.send({ running, connected: snap.status === "running" });
    } catch (err) {
      return reply.send({
        running: [],
        connected: false,
        error: err instanceof Error ? err.message : String(err),
      });
    }
  });

  app.post("/models/pull", async (req, reply) => {
    const body = req.body as { name?: string; url?: string };
    const downloadUrl = body?.url ?? body?.name;
    if (!downloadUrl) {
      return reply.status(400).send({ error: "A GGUF download URL is required (provide url or name)." });
    }
    const ctx = getCtx(app);
    const ggufDir = ctx.config.backend.ggufDirectory;
    const filename = downloadUrl.split("/").pop() ?? "model.gguf";
    const destPath = isAbsolute(ggufDir) ? join(ggufDir, filename) : join(process.cwd(), ggufDir, filename);

    try {
      const res = await fetch(downloadUrl);
      if (!res.ok) throw new Error(`Download failed: ${res.status} ${res.statusText}`);
      if (!res.body) throw new Error("No response body");

      const fileStream = createWriteStream(destPath);
      const reader = res.body.getReader();
      const pump = async () => {
        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          fileStream.write(value);
        }
        fileStream.end();
      };
      await pump();

      await new Promise<void>((resolve, reject) => {
        fileStream.on("finish", resolve);
        fileStream.on("error", reject);
      });

      ctx.events.emit("model:changed", { action: "pull", model: filename });
      return reply.send({ pulled: filename, path: destPath });
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  app.post("/models/load", async (req, reply) => {
    const body = req.body as { model: string };
    const ctx = getCtx(app);
    try {
      await ctx.backend.loadModel(body.model);
      ctx.events.emit("model:changed", { action: "load", model: body.model });
      return reply.send({ loaded: body.model });
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  app.post("/models/unload", async (req, reply) => {
    const body = req.body as { model: string };
    const ctx = getCtx(app);
    try {
      await ctx.backend.unloadModel();
      ctx.events.emit("model:changed", { action: "unload", model: body.model });
      return reply.send({ unloaded: body.model });
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });

  app.delete("/models/:name", async (req, reply) => {
    const name = (req.params as { name: string }).name;
    const ctx = getCtx(app);
    try {
      const models = ctx.backend.listModels();
      const cleanName = normalizeModelName(name);
      const matchName = (n: string) => models.find((m) => normalizeModelName(m.name) === n || m.path.endsWith(n));
      let target = matchName(cleanName);
      if (!target) {
        const unprefixed = stripConnectionPrefix(cleanName);
        if (unprefixed !== cleanName) target = matchName(unprefixed);
      }
      if (!target) return reply.status(404).send({ error: `Model "${name}" not found` });
      unlinkSync(target.path);
      ctx.events.emit("model:changed", { action: "delete", model: name });
      return reply.send({ deleted: name });
    } catch (err: any) {
      return reply.status(502).send({ error: err.message });
    }
  });
}
