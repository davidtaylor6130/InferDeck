import { QueueStore } from "@r9700/gateway-core/QueueStore";
import { ResourceLockManager } from "@r9700/gateway-core/ResourceLockManager";
import { Scheduler } from "@r9700/gateway-core/Scheduler";
import { DatabaseManager } from "./db/database";
import { OllamaProcessManager } from "./services/OllamaProcessManager";
import { EventBus } from "./services/EventBus";
import { HardwareTelemetryService } from "./services/HardwareTelemetry";
import { LogStore } from "./services/LogStore";
import { ManagedServicesRegistry } from "./services/ManagedServiceManager";
import { MetricsStore } from "./services/MetricsStore";
import { WorkloadCoordinator } from "./services/WorkloadCoordinator";
import type { GatewayConfig } from "./config/schema";
import { join, dirname } from "node:path";
import { existsSync } from "node:fs";

function getExeDir(): string {
  return dirname(process.execPath);
}

export class AppContext {
  config: GatewayConfig;
  queueStore: QueueStore;
  lockManager: ResourceLockManager;
  scheduler: Scheduler;
  db: DatabaseManager;
  ollama: OllamaProcessManager;
  events: EventBus;
  logs: LogStore;
  metrics: MetricsStore;
  hardware: HardwareTelemetryService;
  managedServices: ManagedServicesRegistry;
  workloads: WorkloadCoordinator;

  constructor(config: GatewayConfig) {
    this.config = config;
    this.queueStore = new QueueStore();
    this.lockManager = new ResourceLockManager({
      maxConcurrentGpuHeavyJobs: config.scheduler.maxConcurrentGpuHeavyJobs,
    });
    this.scheduler = new Scheduler({
      config: config.scheduler,
      mode: config.modes.startupMode,
      queueStore: this.queueStore,
      onModeChange: (mode) => {
        this.config.modes.startupMode = mode;
      },
    });
    this.ollama = new OllamaProcessManager(config.ollama);
    this.events = new EventBus();

    const dbPath = config.database?.path ?? join(process.cwd(), "data", "gateway.sqlite");
    const exeDir = getExeDir();
    const possibleMigrationsPaths = [
      join(process.cwd(), "db", "migrations"),
      join(process.cwd(), "src", "db", "migrations"),
      join(process.cwd(), "apps", "gateway-service", "src", "db", "migrations"),
      join(exeDir, "db", "migrations"),
      (process as any).resourcesPath ? join((process as any).resourcesPath, "db", "migrations") : "",
    ];

    let migrationsDir = possibleMigrationsPaths[0];
    for (const p of possibleMigrationsPaths) {
      if (existsSync(p)) {
        migrationsDir = p;
        break;
      }
    }
    this.db = new DatabaseManager(dbPath, migrationsDir);
    this.logs = new LogStore(config.logging.dir, this.events);
    this.metrics = new MetricsStore(this.db);
    this.hardware = new HardwareTelemetryService(config.hardware, this.events, this.metrics);
    this.managedServices = new ManagedServicesRegistry(config.managedServices, this.events, this.logs);
    this.workloads = new WorkloadCoordinator(this);
  }

  async init(): Promise<void> {
    await this.db.init();
    await this.ollama.start();
    await this.managedServices.startEnabled();
    this.hardware.start();
    this.restoreQueueFromDatabase();
    this.logs.write({ level: "info", service: "gateway", message: "InferDeck gateway initialized" });
  }

  async shutdown(): Promise<void> {
    this.hardware.stop();
    await this.managedServices.stopAll();
    await this.ollama.stop();
    this.db.close();
  }

  toJSON(): { queueStore: unknown; lockManager: unknown; scheduler: { mode: string } } {
    return {
      queueStore: this.queueStore.getSnapshot(),
      lockManager: this.lockManager.toJSON(),
      scheduler: { mode: this.scheduler.getMode() },
    };
  }

  private restoreQueueFromDatabase(): void {
    try {
      const rows = this.db.client.exec(
        `SELECT id, priority, status FROM jobs WHERE status IN ('queued','paused','running','leased') ORDER BY priority DESC, created_at ASC`
      );
      if (!rows.length) return;
      const [result] = rows;
      for (const row of result.values) {
        const [id, priority, status] = row as [string, number, string];
        this.queueStore.enqueue({
          id,
          priority,
          status: status === "paused" ? "paused" : "queued",
        });
      }
    } catch (err) {
      this.logs.write({ level: "warn", service: "gateway", message: "Could not restore queued jobs", data: { error: err instanceof Error ? err.message : String(err) } });
    }
  }
}
