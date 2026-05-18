import type { DatabaseManager } from "../db/database";

export interface MetricSample {
  source: string;
  metricName: string;
  metricValue: number;
  unit: string | null;
  createdAt: string;
}

export interface MetricsStoreOptions {
  maxRows?: number;
  vacuumIntervalMs?: number;
}

export class MetricsStore {
  private maxRows: number;
  private vacuumTimer: NodeJS.Timeout | null = null;

  constructor(
    private readonly db: DatabaseManager,
    options: MetricsStoreOptions = {}
  ) {
    this.maxRows = options.maxRows ?? 100000;
    const vacuumInterval = options.vacuumIntervalMs ?? 24 * 60 * 60 * 1000; // 24 hours

    // Periodic VACUUM to reclaim disk space from deleted rows
    this.vacuumTimer = setInterval(() => {
      this.vacuum().catch(() => {});
    }, vacuumInterval);
    this.vacuumTimer.unref();
  }

  record(source: string, metricName: string, metricValue: number, unit: string | null = null): void {
    this.db.client.run(
      `INSERT INTO metrics_samples (source, metric_name, metric_value, unit, created_at) VALUES (?, ?, ?, ?, datetime('now'))`,
      [source, metricName, metricValue, unit]
    );

    // Trim oldest rows if over limit
    this.db.client.run(
      `DELETE FROM metrics_samples WHERE id NOT IN (SELECT id FROM metrics_samples ORDER BY created_at DESC LIMIT ?)`,
      [this.maxRows]
    );

    this.db.save();
  }

  list(rangeMinutes = 60): MetricSample[] {
    const stmt = this.db.client.prepare(
      `SELECT source, metric_name as metricName, metric_value as metricValue, unit, created_at as createdAt
       FROM metrics_samples
       WHERE created_at >= datetime('now', '-' || ? || ' minutes')
       ORDER BY created_at ASC`
    );
    stmt.bind([rangeMinutes]);
    const rows: MetricSample[] = [];
    while (stmt.step()) rows.push(stmt.getAsObject() as unknown as MetricSample);
    stmt.free();
    return rows;
  }

  /** Reclaim unused space from deleted rows. Run periodically. */
  private async vacuum(): Promise<void> {
    try {
      this.db.client.exec("VACUUM");
      this.db.save();
    } catch {
      // VACUUM can fail if DB is busy — skip silently
    }
  }

  /** Stop the periodic VACUUM timer. */
  stop(): void {
    if (this.vacuumTimer) {
      clearInterval(this.vacuumTimer);
      this.vacuumTimer = null;
    }
  }
}
