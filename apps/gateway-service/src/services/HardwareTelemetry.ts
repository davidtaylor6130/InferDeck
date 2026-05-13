import { execFile } from "node:child_process";
import { existsSync } from "node:fs";
import { promisify } from "node:util";
import type { GatewayConfig } from "../config/schema";
import type { EventBus } from "./EventBus";
import type { MetricsStore } from "./MetricsStore";

const execFileAsync = promisify(execFile);

export interface HardwareSnapshot {
  available: boolean;
  provider: string;
  reason?: string;
  gpu?: {
    name?: string;
    utilization: number | null;
    memoryUsed: number | null;
    memoryTotal: number | null;
    memoryFree: number | null;
    memoryPercent: number | null;
    temperature: number | null;
    power: number | null;
    fanSpeed: number | null;
    driverVersion?: string | null;
  };
  cpu?: { utilization: number | null };
  memory?: { used: number | null; total: number | null; percentage: number | null };
  disk?: { free: number | null; total: number | null; percentage: number | null };
  timestamp: string;
}

export class HardwareTelemetryService {
  private latest: HardwareSnapshot;
  private timer: NodeJS.Timeout | null = null;

  constructor(
    private readonly config: GatewayConfig["hardware"],
    private readonly events: EventBus,
    private readonly metrics: MetricsStore
  ) {
    this.latest = this.unavailable("not_polled_yet");
  }

  start(): void {
    void this.poll();
    this.timer = setInterval(() => void this.poll(), this.config.pollIntervalMs);
    this.timer.unref?.();
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  getSnapshot(): HardwareSnapshot {
    return this.latest;
  }

  async poll(): Promise<HardwareSnapshot> {
    const provider = this.config.provider;
    if (provider !== "amd_adlx") {
      this.latest = this.unavailable(`provider_${provider}_not_enabled`);
      return this.latest;
    }

    const helperPath = this.config.helperPath;
    if (!helperPath || !existsSync(helperPath)) {
      this.latest = this.unavailable("adlx_helper_not_found");
      this.events.emit("hardware:update", this.latest as unknown as Record<string, unknown>);
      return this.latest;
    }

    try {
      const { stdout } = await execFileAsync(helperPath, ["--json"], { timeout: 2500 });
      const parsed = JSON.parse(stdout) as HardwareSnapshot;
      this.latest = { ...parsed, available: true, provider, timestamp: parsed.timestamp ?? new Date().toISOString() };
      const gpu = this.latest.gpu;
      if (gpu?.utilization != null) this.metrics.record("hardware", "gpu_utilization", gpu.utilization, "%");
      if (gpu?.memoryPercent != null) this.metrics.record("hardware", "vram_usage", gpu.memoryPercent, "%");
      this.events.emit("hardware:update", this.latest as unknown as Record<string, unknown>);
      return this.latest;
    } catch (err) {
      this.latest = this.unavailable(err instanceof Error ? err.message : String(err));
      this.events.emit("hardware:update", this.latest as unknown as Record<string, unknown>);
      return this.latest;
    }
  }

  private unavailable(reason: string): HardwareSnapshot {
    return {
      available: false,
      provider: this.config.provider,
      reason,
      timestamp: new Date().toISOString(),
    };
  }
}
