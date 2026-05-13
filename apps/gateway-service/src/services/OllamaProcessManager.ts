import { spawn, type ChildProcess } from "node:child_process";
import type { GatewayConfig } from "../config/schema";

export type OllamaServiceStatus = "starting" | "running" | "stopped" | "unhealthy" | "error";

export interface OllamaServiceSnapshot {
  id: "ollama";
  name: "Ollama";
  kind: "ollama";
  status: OllamaServiceStatus;
  pid: number | null;
  baseUrl: string;
  managed: boolean;
  version: string | null;
  lastHealthcheckAt: string | null;
  lastError: string | null;
  updatedAt: string;
}

interface OllamaHealthResult {
  healthy: boolean;
  latencyMs: number | null;
  version: string | null;
  error: string | null;
}

export class OllamaProcessManager {
  private readonly config: GatewayConfig["ollama"];
  private child: ChildProcess | null = null;
  private status: OllamaServiceStatus = "stopped";
  private managed = false;
  private version: string | null = null;
  private lastHealthcheckAt: string | null = null;
  private lastError: string | null = null;
  private healthTimer: NodeJS.Timeout | null = null;
  private stopping = false;

  constructor(config: GatewayConfig["ollama"]) {
    this.config = config;
  }

  get baseUrl(): string {
    return this.config.baseUrl.replace(/\/$/, "");
  }

  get isManaged(): boolean {
    return this.managed;
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

    if (!this.config.manageProcess) {
      this.status = "unhealthy";
      this.lastError = health.error;
      this.startHealthLoop();
      return;
    }

    this.spawnProcess();
    this.startHealthLoop();
  }

  async restart(): Promise<OllamaServiceSnapshot> {
    if (!this.config.manageProcess) {
      this.status = "unhealthy";
      this.lastError = "Ollama is configured as an external service and cannot be restarted by the gateway.";
      return this.getSnapshot();
    }

    await this.stopChild();
    this.spawnProcess();
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

  async checkHealth(): Promise<OllamaHealthResult> {
    const started = Date.now();
    try {
      const tagsResponse = await this.fetchWithTimeout(`${this.baseUrl}/api/tags`);
      if (!tagsResponse.ok) {
        throw new Error(`Ollama /api/tags returned ${tagsResponse.status}`);
      }

      let version: string | null = null;
      try {
        const versionResponse = await this.fetchWithTimeout(`${this.baseUrl}/api/version`);
        if (versionResponse.ok) {
          const versionJson = await versionResponse.json() as { version?: string };
          version = versionJson.version ?? null;
        }
      } catch {
        // Version is useful but not required for health.
      }

      this.status = "running";
      this.version = version;
      this.lastError = null;
      this.lastHealthcheckAt = new Date().toISOString();
      return {
        healthy: true,
        latencyMs: Date.now() - started,
        version,
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

  getSnapshot(): OllamaServiceSnapshot {
    return {
      id: "ollama",
      name: "Ollama",
      kind: "ollama",
      status: this.status,
      pid: this.child?.pid ?? null,
      baseUrl: this.baseUrl,
      managed: this.managed,
      version: this.version,
      lastHealthcheckAt: this.lastHealthcheckAt,
      lastError: this.lastError,
      updatedAt: new Date().toISOString(),
    };
  }

  private spawnProcess(): void {
    if (this.child) return;

    this.status = "starting";
    this.managed = true;
    this.lastError = null;

    const env = {
      ...process.env,
      OLLAMA_HOST: this.getOllamaHost(),
    };

    try {
      const child = spawn(this.config.executable, ["serve"], {
        env,
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
      this.lastError = `Ollama exited with ${signal ?? code ?? "unknown"}`;
      if (this.config.restartOnFailure) {
        setTimeout(() => {
          if (!this.stopping && !this.child) this.spawnProcess();
        }, 1000);
      }
    });
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

  private getOllamaHost(): string {
    try {
      const url = new URL(this.baseUrl);
      return `${url.hostname}:${url.port || (url.protocol === "https:" ? "443" : "80")}`;
    } catch {
      return "127.0.0.1:11434";
    }
  }

  private async fetchWithTimeout(url: string): Promise<Response> {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 2000);
    try {
      return await fetch(url, { signal: controller.signal });
    } finally {
      clearTimeout(timeout);
    }
  }
}
