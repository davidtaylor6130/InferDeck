-- Migration 0002: job_events table
INSERT OR IGNORE INTO schema_migrations (name, applied_at)
VALUES ('0002_job_events', datetime('now'));

CREATE TABLE IF NOT EXISTS job_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  job_id TEXT NOT NULL,
  event_type TEXT NOT NULL,
  message TEXT,
  data_json TEXT,
  created_at TEXT NOT NULL DEFAULT (datetime('now')),
  FOREIGN KEY(job_id) REFERENCES jobs(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_job_events_job_id ON job_events(job_id);
CREATE INDEX IF NOT EXISTS idx_job_events_type ON job_events(event_type);
CREATE INDEX IF NOT EXISTS idx_job_events_created ON job_events(created_at);
