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
    const target = this.findModel(models, modelName);
    if (!target) {
      throw new Error(`Model "${modelName}" not found in ${this.config.ggufDirectory}`);
    }

    // same model already running → skip
    if (this.currentModel === target.name && this.child) return;

    // same model but process died → just restart
    if (this.currentModel === target.name) {
      await this.spawnProcess();
      return;
    }

    this.currentModel = target.name;
    if (this.child) {
      await this.stopChild();
    }
    await this.spawnProcess();
  }

  async unloadModel(): Promise<void> {
    this.currentModel = null;
    await this.stopChild();
    this.status = "stopped";
  }

  private findModel(models: LlamaModelInfo[], name: string): LlamaModelInfo | undefined {
    const norm = (s: string) => s.toLowerCase().replace(/[:._\s]/g, "-");
    const normalized = norm(name);
    return models.find((m) => {
      if (m.name === name || m.path.endsWith(name)) return true;
      const basename = m.path.split(/[\\/]/).pop() ?? "";
      if (basename === name) return true;
      const mName = m.name.toLowerCase();
      if (mName === name.toLowerCase() || mName.endsWith(name.toLowerCase())) return true;
      if (mName.includes(name.toLowerCase())) return true;
      // normalized matching (handles "gpt-oss:20b" vs "gpt-oss-20b")
      const mNorm = norm(m.name);
      if (mNorm === normalized || mNorm.endsWith(normalized) || mNorm.includes(normalized)) return true;
      return false;
    });
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
