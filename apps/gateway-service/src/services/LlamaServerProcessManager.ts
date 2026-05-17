import { spawn, type ChildProcess } from "node:child_process";
import { join, isAbsolute } from "node:path";
import { statSync } from "node:fs";
import { scanGgufFiles } from "@r9700/backend-llama/utils";
import { LlamaClient } from "@r9700/backend-llama";
import type { LlamaModelInfo, LlamaHealthResponse } from "@r9700/backend-llama/types";

export interface LlamaConfig {
  enabled: boolean;
  baseUrl: string;
  managed: boolean;
  executable: string;
  ggufDirectory: string;
  model: string | null;
  bindHost: string;
  bindPort: number;
  maxGpuLayers: number;
  noKvOffload: boolean;
  ctxSize: number;
  healthcheckIntervalMs: number;
  restartOnFailure: boolean;
}

export type LlamaServiceStatus = "starting" | "running" | "stopped" | "unhealthy" | "error";

export interface LlamaServiceSnapshot {
  id: "llama-server";
  name: "llama.cpp";
  kind: "llama_cpp";
  status: LlamaServiceStatus;
  pid: number | null;
  baseUrl: string;
  managed: boolean;
  model: string | null;
  version: string | null;
  lastHealthcheckAt: string | null;
  lastError: string | null;
  updatedAt: string;
}

interface HealthResult {
  healthy: boolean;
  latencyMs: number | null;
  version: string | null;
  error: string | null;
}

/** Strip :latest tag and .gguf extension from a model name */
export function normalizeModelName(name: string): string {
  return name
    .replace(/:latest$/i, "")      // strip :latest tag
    .replace(/\.gguf$/i, "");      // strip .gguf extension
}

/** Convert a GGUF file path to a clean Ollama-style model name (e.g. "qwen3-coder-30b-a3b-instruct:latest") */
export function toOllamaModelName(ggufPath: string): string {
  const basename = ggufPath.split(/[\\/]/).pop()?.replace(/\.gguf$/i, "") ?? ggufPath;
  // Remove quantization suffix (handles -Q2_K, .Q2_K, _Q2_K, -Q4_K_M, -UD-IQ3_XXS, -F32, -MXFP4, etc.)
  const cleaned = basename.replace(/[-_.](Q[0-9]+[_][A-Z0-9_]+|UD[-_.][A-Z0-9_]+|F32|F16|BF16|MXFP4|Q[0-9]+_K|Q8_0)$/gi, "");
  return cleaned.toLowerCase() + ":latest";
}

/** Try stripping an Open Web UI connection prefix (e.g. "asdf.modelname" -> "modelname") */
export function stripConnectionPrefix(name: string): string {
  return name.replace(/^[^.]+\./, "");
}

export class LlamaServerProcessManager {
  private readonly config: LlamaConfig;
  private readonly client: LlamaClient;
  private child: ChildProcess | null = null;
  private status: LlamaServiceStatus = "stopped";
  private managed = false;
  private version: string | null = null;
  private currentModel: string | null = null;
  private lastHealthcheckAt: string | null = null;
  private lastError: string | null = null;
  private healthTimer: NodeJS.Timeout | null = null;
  private stopping = false;

  constructor(config: LlamaConfig) {
    this.config = config;
    this.client = new LlamaClient({ baseUrl: config.baseUrl, timeoutMs: 5000 });
    this.currentModel = config.model;
  }

  get baseUrl(): string {
    return this.config.baseUrl.replace(/\/$/, "");
  }

  get isManaged(): boolean {
    return this.managed;
  }

  get activeModel(): string | null {
    return this.currentModel;
  }

  async start(): Promise<void> {
    if (!this.config.enabled) {
      this.status = "stopped";
      this.lastError = null;
      return;
    }

    const health = await this.checkHealth();
    if (health.healthy) {
      this.status = "running";
      this.managed = false;
      console.log("[backend] Found already running at", this.config.baseUrl);
      this.startHealthLoop();
      return;
    }

    console.log("[backend] Health check failed:", health.error);

    if (!this.config.managed) {
      this.status = "unhealthy";
      this.lastError = health.error;
      console.log("[backend] Not managed, will not spawn");
      this.startHealthLoop();
      return;
    }

    console.log("[backend] Managed mode, spawning process...");
    await this.spawnProcess();
    this.startHealthLoop();
  }

  async restart(): Promise<LlamaServiceSnapshot> {
    if (!this.config.managed) {
      this.status = "unhealthy";
      this.lastError = "llama.cpp is configured as an external service and cannot be restarted by the gateway.";
      return this.getSnapshot();
    }

    await this.stopChild();
    await this.spawnProcess();
    return this.getSnapshot();
  }

  async stop(): Promise<void> {
    this.stopping = true;
    if (this.healthTimer) {
      clearInterval(this.healthTimer);
      this.healthTimer = null;
    }
    await this.stopChild();
    this.status = "stopped";
  }

  async loadModel(modelName: string): Promise<void> {
    const models = scanGgufFiles(this.config.ggufDirectory);
    if (models.length === 0) {
      throw new Error(`No GGUF models found in ${this.config.ggufDirectory}`);
    }

    // If a model is already loaded and running, accept the request without restarting
    if (this.currentModel && this.child) {
      const target = this.findModel(models, modelName);
      if (target && target.name === this.currentModel) {
        console.log(`[backend] loadModel skip: "${this.currentModel}" already loaded and running`);
        return;
      }
      // Even if name doesn't match exactly, if we have a model loaded, allow it
      // Open WebUI may send names with prefixes/tags that don't match exactly
      console.log(`[backend] loadModel accept: "${modelName}" -> using already loaded "${this.currentModel}"`);
      return;
    }

    const target = this.findModel(models, modelName);
    if (!target) {
      // Fallback: use the first available model if requested name not found
      console.warn(`[backend] Model "${modelName}" not found, falling back to first available: "${models[0].name}"`);
      this.currentModel = models[0].name;
    } else {
      this.currentModel = target.name;
    }

    await this.spawnProcess();
  }

  async unloadModel(): Promise<void> {
    this.currentModel = null;
    await this.stopChild();
    this.status = "stopped";
  }

  private findModel(models: LlamaModelInfo[], name: string): LlamaModelInfo | undefined {
    const norm = (s: string) => s.toLowerCase().replace(/[:._\s\/\\-]/g, "");

    const tryMatch = (n: string): LlamaModelInfo | undefined => {
      const cleaned = normalizeModelName(n);
      const normalized = norm(cleaned);

      return models.find((m) => {
        const mNameClean = normalizeModelName(m.name);
        const mNorm = norm(mNameClean);
        const basename = m.path.split(/[\\/]/).pop()?.replace(/\.gguf$/i, "") ?? "";
        const bNorm = norm(basename);
        const ollamaName = toOllamaModelName(m.path).replace(/:latest$/i, "");
        const oNorm = norm(ollamaName);

        // Exact match (normalized)
        if (mNameClean === cleaned) return true;
        // Ollama-style name match
        if (ollamaName === cleaned || oNorm === normalized) return true;
        // Path ends with cleaned name
        if (m.path.endsWith(cleaned)) return true;
        // Basename match
        if (normalizeModelName(basename) === cleaned) return true;
        // Case-insensitive exact match
        if (mNameClean.toLowerCase() === cleaned.toLowerCase()) return true;
        // Ends with (case-insensitive)
        if (mNameClean.toLowerCase().endsWith(cleaned.toLowerCase())) return true;
        // Normalized comparison (strips all special chars)
        if (mNorm === normalized) return true;
        if (mNorm.endsWith(normalized) || mNorm.includes(normalized)) return true;
        if (normalized.endsWith(mNorm) || normalized.includes(mNorm)) return true;
        // Basename normalized match
        if (bNorm === normalized || bNorm.includes(normalized) || normalized.includes(bNorm)) return true;
        return false;
      });
    };

    // Try full name first
    let found = tryMatch(name);
    if (found) return found;

    // Strip Open WebUI connection prefix (e.g. "conn-id.modelname")
    const prefixed = stripConnectionPrefix(name);
    if (prefixed !== name) {
      found = tryMatch(prefixed);
      if (found) return found;
    }

    // Try just the filename part (after last /)
    const lastSlash = name.lastIndexOf("/");
    if (lastSlash !== -1) {
      found = tryMatch(name.substring(lastSlash + 1));
      if (found) return found;
    }

    return undefined;
  }

  listModels(): LlamaModelInfo[] {
    return scanGgufFiles(this.config.ggufDirectory);
  }

  async checkHealth(): Promise<HealthResult> {
    const started = Date.now();
    try {
      const healthData: LlamaHealthResponse = await this.client.health();

      this.status = "running";
      this.lastError = null;
      this.lastHealthcheckAt = new Date().toISOString();
      return {
        healthy: true,
        latencyMs: Date.now() - started,
        version: healthData.model_info ? `llama.cpp (ctx: ${healthData.model_info.n_ctx ?? "?"})` : "llama.cpp",
        error: null,
      };
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      this.status = this.child ? "starting" : "unhealthy";
      this.lastError = message;
      this.lastHealthcheckAt = new Date().toISOString();
      console.error(`[backend] Health check failed: ${message}`);
      return {
        healthy: false,
        latencyMs: Date.now() - started,
        version: null,
        error: message,
      };
    }
  }

  getSnapshot(): LlamaServiceSnapshot {
    return {
      id: "llama-server",
      name: "llama.cpp",
      kind: "llama_cpp",
      status: this.status,
      pid: this.child?.pid ?? null,
      baseUrl: this.baseUrl,
      managed: this.managed,
      model: this.currentModel,
      version: this.version,
      lastHealthcheckAt: this.lastHealthcheckAt,
      lastError: this.lastError,
      updatedAt: new Date().toISOString(),
    };
  }

  private async spawnProcess(): Promise<void> {
    if (this.child) return;

    const modelPath = this.resolveModelPath();
    if (!modelPath) {
      this.status = "error";
      const msg = `No GGUF model found in ${this.config.ggufDirectory}. Place a .gguf file or set backend.model in config.`;
      this.lastError = msg;
      console.error("[backend]", msg);
      return;
    }

    this.status = "starting";
    this.managed = true;
    this.lastError = null;

    const args = [
      "-m", modelPath,
      "--host", this.config.bindHost,
      "--port", String(this.config.bindPort),
      "-ngl", String(this.config.maxGpuLayers),
    ];
    if (this.config.ctxSize > 0) args.push("-c", String(this.config.ctxSize));
    if (this.config.noKvOffload) args.push("--no-kv-offload");

    console.log(`[backend] Spawning: ${this.config.executable} ${args.join(" ")}`);

    try {
      const child = spawn(this.config.executable, args, {
        stdio: ["ignore", "pipe", "pipe"],
      });
      this.child = child;
      console.log(`[backend] Spawned pid=${child.pid}`);
    } catch (err) {
      this.status = "error";
      this.lastError = err instanceof Error ? err.message : String(err);
      this.child = null;
      console.error("[backend] Spawn failed:", this.lastError);
      return;
    }

    this.child.stderr?.on("data", (chunk) => {
      const text = String(chunk).trim();
      if (text) {
        this.lastError = text.slice(-500);
        console.error("[backend:stderr]", text);
      }
    });

    this.child.on("error", (err) => {
      this.status = "error";
      this.lastError = err.message;
      this.child = null;
      console.error("[backend] Process error:", err.message);
    });

    this.child.on("exit", (code, signal) => {
      const pid = this.child?.pid;
      this.child = null;
      console.log(`[backend] exit event: pid=${pid} code=${code} signal=${signal} stopping=${this.stopping} currentModel=${this.currentModel}`);
      if (this.stopping) return;

      this.status = "stopped";
      this.lastError = `llama-server exited with ${signal ?? code ?? "unknown"}`;
      console.error(`[backend] Process exited pid=${pid} code=${code} signal=${signal}`);
      if (this.config.restartOnFailure && this.currentModel) {
        setTimeout(() => {
          if (!this.stopping && !this.child) void this.spawnProcess();
        }, 1000);
      }
    });
  }

  private resolveModelPath(): string | null {
    // use explicit config.model if set
    if (this.config.model) {
      const candidate = isAbsolute(this.config.model)
        ? this.config.model
        : join(this.config.ggufDirectory, this.config.model);
      try {
        if (statSync(candidate).isFile()) {
          this.currentModel = this.config.model;
          return candidate;
        }
      } catch {
        // not found
      }
    }

    const models = scanGgufFiles(this.config.ggufDirectory);
    if (models.length === 0) return null;

    // respect currentModel if already set (e.g., from loadModel or startup)
    const cm = this.currentModel;
    if (cm) {
      const current = models.find((m) => m.name === cm || m.path.endsWith(cm));
      if (current) return current.path;
      console.warn(`[backend] Current model "${cm}" not found in GGUF directory, will pick a model`);
    }

    // if config.model is set, find matching model by name
    const preferred = this.config.model;
    if (preferred) {
      const matched = this.findModel(models, preferred);
      if (matched) {
        this.currentModel = matched.name;
        return matched.path;
      }
      console.warn(`[backend] Model "${preferred}" not found, falling back to first available`);
    }

    const first = models[0];
    this.currentModel = first.name;
    return first.path;
  }

  private startHealthLoop(): void {
    if (this.healthTimer) return;
    this.healthTimer = setInterval(() => {
      void this.checkHealth();
    }, this.config.healthcheckIntervalMs);
    this.healthTimer.unref?.();
  }

  private async stopChild(): Promise<void> {
    const child = this.child;
    if (!child) return;

    await new Promise<void>((resolve) => {
      const timeout = setTimeout(() => {
        child.kill("SIGKILL");
        resolve();
      }, 5000);
      child.once("exit", () => {
        clearTimeout(timeout);
        resolve();
      });
      child.kill("SIGTERM");
    });
    this.child = null;
  }
}
