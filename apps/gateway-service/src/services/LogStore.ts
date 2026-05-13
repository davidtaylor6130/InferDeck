import { existsSync, mkdirSync, readFileSync, appendFileSync, readdirSync, statSync } from "node:fs";
import { join } from "node:path";
import type { EventBus } from "./EventBus";

export type LogLevel = "debug" | "info" | "warn" | "error";

export interface LogEntry {
  timestamp: string;
  level: LogLevel;
  service: string;
  message: string;
  jobId?: string | null;
  data?: Record<string, unknown>;
}

export class LogStore {
  constructor(private readonly dir: string, private readonly events?: EventBus) {
    if (!existsSync(dir)) mkdirSync(dir, { recursive: true });
  }

  write(entry: Omit<LogEntry, "timestamp"> & { timestamp?: string }): void {
    const full: LogEntry = {
      timestamp: entry.timestamp ?? new Date().toISOString(),
      level: entry.level,
      service: entry.service,
      message: entry.message,
      jobId: entry.jobId ?? null,
      data: entry.data,
    };
    appendFileSync(this.pathFor(entry.service === "service-error" ? "service-errors" : "gateway"), `${JSON.stringify(full)}\n`);
    this.events?.emit("log:entry", full as unknown as Record<string, unknown>);
  }

  list(query: { tab?: string; level?: string; service?: string; jobId?: string; search?: string; limit?: number }): LogEntry[] {
    const file = query.tab === "service-errors" ? "service-errors.log" : "gateway.log";
    const path = join(this.dir, file);
    if (!existsSync(path)) return [];
    const lines = readFileSync(path, "utf-8").split(/\r?\n/).filter(Boolean).slice(-(query.limit ?? 500));
    return lines
      .map((line) => {
        try { return JSON.parse(line) as LogEntry; } catch { return null; }
      })
      .filter((entry): entry is LogEntry => Boolean(entry))
      .filter((entry) => !query.level || query.level === "all" || entry.level === query.level)
      .filter((entry) => !query.service || query.service === "all" || entry.service === query.service)
      .filter((entry) => !query.jobId || entry.jobId === query.jobId)
      .filter((entry) => !query.search || JSON.stringify(entry).toLowerCase().includes(query.search.toLowerCase()));
  }

  stats(): { dir: string; totalBytes: number } {
    let totalBytes = 0;
    if (existsSync(this.dir)) {
      for (const file of readdirSync(this.dir)) {
        const path = join(this.dir, file);
        try {
          const stat = statSync(path);
          if (stat.isFile()) totalBytes += stat.size;
        } catch {}
      }
    }
    return { dir: this.dir, totalBytes };
  }

  private pathFor(name: string): string {
    return join(this.dir, `${name}.log`);
  }
}
