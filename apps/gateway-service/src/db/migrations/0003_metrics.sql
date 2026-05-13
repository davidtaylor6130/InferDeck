-- Migration 0003: metrics_samples table
INSERT OR IGNORE INTO schema_migrations (name, applied_at)
VALUES ('0003_metrics', datetime('now'));

CREATE TABLE IF NOT EXISTS metrics_samples (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  source TEXT NOT NULL,
  metric_name TEXT NOT NULL,
  metric_value REAL NOT NULL,
  unit TEXT,
  created_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_metrics_source ON metrics_samples(source);
CREATE INDEX IF NOT EXISTS idx_metrics_name ON metrics_samples(metric_name);
CREATE INDEX IF NOT EXISTS idx_metrics_created ON metrics_samples(created_at);
