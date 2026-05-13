import { spawn, type ChildProcess } from "node:child_process";
import type { GatewayConfig } from "../config/schema";
import type { EventBus } from "./EventBus";
import type { LogStore } from "./LogStore";

export interface ManagedServiceSnapshot {
  id: string;
  name: string;
  kind: string;
  status: "not_configured" | "starting" | "running" | "stopped" | "unhealthy" | "error";
  pid: number | null;
  baseUrl: string | null;
  managed: boolean;
  enabled: boolean;
  lastHealthcheckAt: string | null;
  lastError: string | null;
  updatedAt: string;
}

type ManagedServiceConfig = GatewayConfig["managedServices"][number];

export class ManagedServiceManager {
  private child: ChildProcess | null = null;
  private status: ManagedServiceSnapshot["status"];
  private lastHealthcheckAt: string | null = null;
  private lastError: string | null = null;
  private healthTimer: NodeJS.Timeout | null = null;
  private stopping = false;

  constructor(
    private readonly config: ManagedServiceConfig,
    private readonly events: EventBus,
    private readonly logs: LogStore
  ) {
    this.status = config.enabled && config.command ? "stopped" : "not_configured";
  }

  get id(): string {
    return this.config.id;
  }

  async start(): Promise<ManagedServiceSnapshot> {
    if (!this.config.enabled || !this.config.command) {
      this.status = "not_configured";
      this.lastError = "Service is disabled or command is not configured.";
      return this.getSnapshot();
    }

    const health = await this.checkHealth();
    if (health) {
      this.status = "running";
      this.startHealthLoop();
      return this.getSnapshot();
    }

    this.spawnProcess();
    this.startHealthLoop();
    return this.getSnapshot();
  }

  async stop(): Promise<ManagedServiceSnapshot> {
    this.stopping = true;
    if (this.healthTimer) clearInterval(this.healthTimer);
    this.healthTimer = null;
    await this.stopChild();
    this.status = this.config.enabled ? "stopped" : "not_configured";
    this.emit();
    return this.getSnapshot();
  }

  async restart(): Promise<ManagedServiceSnapshot> {
    await this.stop();
    this.stopping = false;
    return this.start();
  }

  async checkHealth(): Promise<boolean> {
    this.lastHealthcheckAt = new Date().toISOString();
    if (!this.config.healthUrl) {
      const running = Boolean(this.child && !this.child.killed);
      this.status = running ? "running" : this.status;
      this.emit();
      return running;
    }

    try {
      const controller = new AbortController();
      const timeout = setTimeout(() => controller.abort(), 2000);
      const res = await fetch(this.config.healthUrl, { signal: controller.signal });
      clearTimeout(timeout);
      if (!res.ok) throw new Error(`health returned ${res.status}`);
      this.status = "running";
      this.lastError = null;
      this.emit();
      return true;
    } catch (err) {
      this.status = this.child ? "starting" : this.config.enabled ? "unhealthy" : "not_configured";
      this.lastError = err instanceof Error ? err.message : String(err);
      this.emit();
      return false;
    }
  }

  getSnapshot(): ManagedServiceSnapshot {
    return {
      id: this.config.id,
      name: this.config.name,
      kind: this.config.kind,
      status: this.status,
      pid: this.child?.pid ?? null,
      baseUrl: this.config.baseUrl ?? null,
      managed: Boolean(this.config.enabled && this.config.command),
      enabled: this.config.enabled,
      lastHealthcheckAt: this.lastHealthcheckAt,
      lastError: this.lastError,
      updatedAt: new Date().toISOString(),
    };
  }

  private spawnProcess(): void {
    if (this.child) return;
    this.status = "starting";
    this.lastError = null;
    try {
      const child = spawn(this.config.command, this.config.args, {
        cwd: this.config.cwd,
        env: { ...process.env, ...this.config.env },
        stdio: ["ignore", "pipe", "pipe"],
      });
      this.child = child;
      child.stderr?.on("data", (chunk) => this.capture("error", String(chunk)));
      child.stdout?.on("data", (chunk) => this.capture("info", String(chunk)));
      child.on("error", (err) => {
        this.status = "error";
        this.lastError = err.message;
        this.child = null;
        this.emit();
      });
      child.on("exit", (code, signal) => {
        this.child = null;
        if (this.stopping) return;
        this.status = "stopped";
        this.lastError = `Exited with ${signal ?? code ?? "unknown"}`;
        this.emit();
        if (this.config.restartOnFailure) setTimeout(() => void this.start(), 1000);
      });
    } catch (err) {
      this.status = "error";
      this.lastError = err instanceof Error ? err.message : String(err);
      this.emit();
    }
  }

  private capture(level: "info" | "error", chunk: string): void {
    const message = chunk.trim();
    if (!message) return;
    if (level === "error") this.lastError = message.slice(-500);
    this.logs.write({ level, service: level === "error" ? "service-error" : this.config.id, message, data: { serviceId: this.config.id } });
  }

  private startHealthLoop(): void {
    if (this.healthTimer) return;
    this.healthTimer = setInterval(() => void this.checkHealth(), 5000);
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

  private emit(): void {
    this.events.emit("service:health", this.getSnapshot() as unknown as Record<string, unknown>);
  }
}

export class ManagedServicesRegistry {
  private services = new Map<string, ManagedServiceManager>();

  constructor(configs: GatewayConfig["managedServices"], events: EventBus, logs: LogStore) {
    for (const config of configs) {
      this.services.set(config.id, new ManagedServiceManager(config, events, logs));
    }
  }

  async startEnabled(): Promise<void> {
    await Promise.all(Array.from(this.services.values()).map((service) => service.start()));
  }

  async stopAll(): Promise<void> {
    await Promise.all(Array.from(this.services.values()).map((service) => service.stop()));
  }

  list(): ManagedServiceSnapshot[] {
    return Array.from(this.services.values()).map((service) => service.getSnapshot());
  }

  get(id: string): ManagedServiceManager | null {
    return this.services.get(id) ?? null;
  }
}
