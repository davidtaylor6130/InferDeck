-- Migration 0001: Init schema
CREATE TABLE IF NOT EXISTS schema_migrations (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE,
  applied_at TEXT NOT NULL DEFAULT (datetime('now'))
);

INSERT OR IGNORE INTO schema_migrations (name, applied_at)
VALUES ('0001_init', datetime('now'));

-- Jobs table
CREATE TABLE IF NOT EXISTS jobs (
  id TEXT PRIMARY KEY,
  type TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'queued',
  priority INTEGER NOT NULL DEFAULT 50,
  resource_class TEXT NOT NULL DEFAULT 'none',
  client_name TEXT,
  request_path TEXT,
  request_method TEXT,
  payload_json TEXT NOT NULL DEFAULT '{}',
  result_json TEXT,
  error_json TEXT,
  created_at TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at TEXT NOT NULL DEFAULT (datetime('now')),
  started_at TEXT,
  finished_at TEXT,
  lease_until TEXT,
  retry_count INTEGER NOT NULL DEFAULT 0,
  max_retries INTEGER NOT NULL DEFAULT 2,
  idempotency_key TEXT,
  CHECK(status IN ('queued','leased','running','paused','succeeded','failed','cancelled','dead_letter')),
  CHECK(resource_class IN ('gpu_llm','gpu_image','gpu_audio','cpu_index','disk_heavy','network','none'))
);

CREATE INDEX IF NOT EXISTS idx_jobs_status ON jobs(status);
CREATE INDEX IF NOT EXISTS idx_jobs_priority ON jobs(priority DESC);
CREATE INDEX IF NOT EXISTS idx_jobs_created ON jobs(created_at);
CREATE INDEX IF NOT EXISTS idx_jobs_type ON jobs(type);
CREATE INDEX IF NOT EXISTS idx_jobs_lease ON jobs(lease_until);
CREATE INDEX IF NOT EXISTS idx_jobs_status_priority ON jobs(status, priority DESC, created_at);

-- Services table
CREATE TABLE IF NOT EXISTS services (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  kind TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'unknown',
  pid INTEGER,
  base_url TEXT,
  last_healthcheck_at TEXT,
  last_error TEXT,
  updated_at TEXT NOT NULL DEFAULT (datetime('now')),
  metadata_json TEXT,
  CHECK(status IN ('unknown','starting','ready','running','stopped','error','unhealthy'))
);

INSERT OR IGNORE INTO services (id, name, kind, status, updated_at) VALUES
  ('gateway', 'AI Gateway', 'gateway', 'running', datetime('now'));

-- Settings table
CREATE TABLE IF NOT EXISTS settings (
  key TEXT PRIMARY KEY,
  value_json TEXT NOT NULL DEFAULT '{}',
  updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);

INSERT OR IGNORE INTO settings (key, value_json, updated_at)
VALUES ('current_mode', '{"mode":"ai"}', datetime('now'));

-- Create WAL mode
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA cache_size=-64000;
PRAGMA temp_store=MEMORY;
