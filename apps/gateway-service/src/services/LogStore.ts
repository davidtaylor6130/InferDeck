import { existsSync, mkdirSync, appendFileSync, readdirSync, statSync, renameSync, unlinkSync } from "node:fs";
import { join } from "node:path";
import { createReadStream } from "node:fs";
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

export interface LogStoreOptions {
  maxFileSizeBytes?: number;
  maxRotatedFiles?: number;
  retentionDays?: number;
}

const MAX_FILE_SIZE = 50 * 1024 * 1024; // 50MB
const MAX_ROTATED_FILES = 10;

export class LogStore {
  private maxFileSize: number;
  private maxRotatedFiles: number;
  private retentionDays: number;
  private rotationTimers: Map<string, NodeJS.Timeout> = new Map();

  constructor(
    private readonly dir: string,
    private readonly events?: EventBus,
    options: LogStoreOptions = {}
  ) {
    if (!existsSync(dir)) mkdirSync(dir, { recursive: true });
    this.maxFileSize = options.maxFileSizeBytes ?? MAX_FILE_SIZE;
    this.maxRotatedFiles = options.maxRotatedFiles ?? MAX_ROTATED_FILES;
    this.retentionDays = options.retentionDays ?? 30;

    // Periodic cleanup of old rotated logs
    const cleanupTimer = setInterval(() => {
      this.cleanupOldLogs().catch(() => {});
    }, 60 * 60 * 1000); // Every hour
    cleanupTimer.unref();
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

    const logName = entry.service === "service-error" ? "service-errors" : "gateway";
    const filePath = this.pathFor(logName);

    // Check if rotation is needed before writing
    this.maybeRotate(filePath);

    appendFileSync(filePath, `${JSON.stringify(full)}\n`);
    this.events?.emit("log:entry", full as unknown as Record<string, unknown>);
  }

  /** Read the last N lines from a log file using streaming to avoid OOM on large files. */
  list(query: { tab?: string; level?: string; service?: string; jobId?: string; search?: string; limit?: number }): LogEntry[] {
    const file = query.tab === "service-errors" ? "service-errors.log" : "gateway.log";
    const path = join(this.dir, file);
    if (!existsSync(path)) return [];

    const limit = query.limit ?? 500;
    const lines = this.readLastLines(path, limit);

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

  /** Read the last N lines of a file without loading the entire file into memory. */
  private readLastLines(filePath: string, count: number): string[] {
    try {
      const stat = statSync(filePath);
      if (stat.size === 0) return [];

      // Start reading from near the end, expand if not enough lines
      let bufferSize = Math.min(64 * 1024, stat.size); // Start with 64KB
      let lines: string[] = [];

      while (lines.length < count && bufferSize < stat.size) {
        const start = Math.max(0, stat.size - bufferSize);
        const chunk = this.readChunkSync(filePath, start, bufferSize);
        lines = chunk.split(/\r?\n/).filter(Boolean).slice(-count);
        if (lines.length >= count) break;
        bufferSize = Math.min(bufferSize * 2, stat.size);
      }

      return lines.slice(-count);
    } catch {
      return [];
    }
  }

  /** Synchronously read a chunk of a file from a given position. */
  private readChunkSync(filePath: string, start: number, length: number): string {
    const fd = require("node:fs").openSync(filePath, "r");
    const buffer = Buffer.alloc(length);
    try {
      require("node:fs").readSync(fd, buffer, 0, length, start);
      return buffer.toString("utf-8");
    } finally {
      require("node:fs").closeSync(fd);
    }
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

  /** Rotate a log file if it exceeds the max size. */
  private maybeRotate(filePath: string): void {
    if (!existsSync(filePath)) return;
    const stat = statSync(filePath);
    if (stat.size < this.maxFileSize) return;

    try {
      // Shift existing rotated files: .log.3 → .log.4, .log.2 → .log.3, etc.
      for (let i = this.maxRotatedFiles - 1; i >= 1; i--) {
        const oldPath = `${filePath}.${i}`;
        const newPath = `${filePath}.${i + 1}`;
        if (existsSync(oldPath)) {
          if (i + 1 > this.maxRotatedFiles) {
            unlinkSync(oldPath);
          } else {
            renameSync(oldPath, newPath);
          }
        }
      }

      // Rotate current file
      renameSync(filePath, `${filePath}.1`);
    } catch (err) {
      console.error(`[log] Failed to rotate ${filePath}:`, err);
    }
  }

  /** Delete rotated log files older than retentionDays. */
  private async cleanupOldLogs(): Promise<void> {
    try {
      const now = Date.now();
      const cutoff = now - this.retentionDays * 24 * 60 * 60 * 1000;

      for (const file of readdirSync(this.dir)) {
        if (!file.includes(".log.")) continue; // Only rotated files
        const filePath = join(this.dir, file);
        const stat = statSync(filePath);
        if (stat.mtimeMs < cutoff) {
          unlinkSync(filePath);
        }
      }
    } catch {
      // Ignore cleanup errors
    }
  }
}
