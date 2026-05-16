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
      this.startHealthLoop();
      return;
    }

    if (!this.config.managed) {
      this.status = "unhealthy";
      this.lastError = health.error;
      this.startHealthLoop();
      return;
    }

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
    const target = models.find((m) => m.name === modelName || m.path.endsWith(modelName));
    if (!target) {
      throw new Error(`Model "${modelName}" not found in ${this.config.ggufDirectory}`);
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
      this.lastError = `No GGUF model found in ${this.config.ggufDirectory}. Place a .gguf file or set backend.model in config.`;
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

    try {
      const child = spawn(this.config.executable, args, {
        stdio: ["ignore", "pipe", "pipe"],
      });
      this.child = child;
    } catch (err) {
      this.status = "error";
      this.lastError = err instanceof Error ? err.message : String(err);
      this.child = null;
      return;
    }

    this.child.stderr?.on("data", (chunk) => {
      const text = String(chunk).trim();
      if (text) this.lastError = text.slice(-500);
    });

    this.child.on("error", (err) => {
      this.status = "error";
      this.lastError = err.message;
      this.child = null;
    });

    this.child.on("exit", (code, signal) => {
      this.child = null;
      if (this.stopping) return;

      this.status = "stopped";
      this.lastError = `llama-server exited with ${signal ?? code ?? "unknown"}`;
      if (this.config.restartOnFailure && this.currentModel) {
        setTimeout(() => {
          if (!this.stopping && !this.child) void this.spawnProcess();
        }, 1000);
      }
    });
  }

  private resolveModelPath(): string | null {
    if (this.currentModel) {
      const candidate = isAbsolute(this.currentModel)
        ? this.currentModel
        : join(this.config.ggufDirectory, this.currentModel);
      try {
        if (statSync(candidate).isFile()) return candidate;
      } catch {
        // not found at exact path
      }
    }

    const models = scanGgufFiles(this.config.ggufDirectory);
    if (models.length === 0) return null;

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
