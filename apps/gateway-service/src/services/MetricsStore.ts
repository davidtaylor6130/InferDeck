import type { DatabaseManager } from "../db/database";

export interface MetricSample {
  source: string;
  metricName: string;
  metricValue: number;
  unit: string | null;
  createdAt: string;
}

export class MetricsStore {
  constructor(private readonly db: DatabaseManager) {}

  record(source: string, metricName: string, metricValue: number, unit: string | null = null): void {
    this.db.client.run(
      `INSERT INTO metrics_samples (source, metric_name, metric_value, unit, created_at) VALUES (?, ?, ?, ?, datetime('now'))`,
      [source, metricName, metricValue, unit]
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
}
