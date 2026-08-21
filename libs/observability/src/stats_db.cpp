#include "observability/stats_db.hpp"

#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include "foundation/path_utils.hpp"

namespace inferdeck::observability {

namespace {

constexpr int current_schema_version = 2;

void throw_on_error(int rc, sqlite3* db, const char* what) {
  if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
    std::string msg = std::string("sqlite: ") + what + ": " + (db ? sqlite3_errmsg(db) : "?");
    throw std::runtime_error(msg);
  }
}

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool table_exists(sqlite3* db, const char* name) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
      "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;",
      -1, &stmt, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
  const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return exists;
}

bool backup_database(sqlite3* source, const std::string& path) {
  sqlite3* destination = nullptr;
  if (sqlite3_open(path.c_str(), &destination) != SQLITE_OK) {
    if (destination) sqlite3_close(destination);
    return false;
  }
  sqlite3_backup* backup = sqlite3_backup_init(destination, "main", source, "main");
  const bool ok = backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE;
  if (backup) sqlite3_backup_finish(backup);
  sqlite3_close(destination);
  return ok;
}

}

StatsDb::StatsDb(const std::string& path)
    : path_(path == ":memory:"
        ? path
        : foundation::expand_user_path(std::filesystem::path(path)).string()) {
  open();
}
StatsDb::~StatsDb() { close(); }

void StatsDb::open() {
  std::lock_guard lk(mtx_);
  if (path_.empty()) {
    healthy_ = false;
    return;
  }
  if (path_ != ":memory:") {
    std::error_code ec;
    const auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
  }
  if (sqlite3_open(path_.c_str(), reinterpret_cast<sqlite3**>(&db_)) != SQLITE_OK) {
    healthy_ = false;
    return;
  }
  sqlite3_busy_timeout(reinterpret_cast<sqlite3*>(db_), 5000);
  auto* db = reinterpret_cast<sqlite3*>(db_);
  char* pragma_err = nullptr;
  if (sqlite3_exec(reinterpret_cast<sqlite3*>(db_),
                   "PRAGMA journal_mode=WAL;"
                   "PRAGMA synchronous=NORMAL;"
                   "PRAGMA temp_store=MEMORY;",
                   nullptr, nullptr, &pragma_err) != SQLITE_OK) {
    if (pragma_err) sqlite3_free(pragma_err);
    healthy_ = false;
    return;
  }
  int schema_version = 0;
  sqlite3_stmt* version_stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &version_stmt, nullptr) == SQLITE_OK &&
      sqlite3_step(version_stmt) == SQLITE_ROW) {
    schema_version = sqlite3_column_int(version_stmt, 0);
  }
  sqlite3_finalize(version_stmt);
  if (schema_version > current_schema_version) {
    healthy_ = false;
    return;
  }
  if (path_ != ":memory:" && schema_version < current_schema_version &&
      table_exists(db, "requests") &&
      !backup_database(db, path_ + ".backup-v" + std::to_string(schema_version))) {
    healthy_ = false;
    return;
  }
  if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    healthy_ = false;
    return;
  }
  const char* schema =
    "CREATE TABLE IF NOT EXISTS requests ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts INTEGER NOT NULL,"
    "  model TEXT NOT NULL,"
    "  resolved_model TEXT NOT NULL DEFAULT '',"
    "  prompt_tokens INTEGER NOT NULL,"
    "  completion_tokens INTEGER NOT NULL,"
    "  duration_ms REAL NOT NULL,"
    "  tps REAL NOT NULL,"
    "  status_code INTEGER NOT NULL,"
    "  slot_id INTEGER NOT NULL,"
    "  input_audio_seconds REAL NOT NULL DEFAULT 0,"
    "  input_characters INTEGER NOT NULL DEFAULT 0,"
    "  cached_prompt_tokens INTEGER NOT NULL DEFAULT 0,"
    "  generation_duration_ms REAL NOT NULL DEFAULT 0,"
    "  prompt_duration_ms REAL NOT NULL DEFAULT 0,"
    "  prompt_tps REAL NOT NULL DEFAULT 0"
    "  ,request_id TEXT NOT NULL DEFAULT ''"
    "  ,principal_class TEXT NOT NULL DEFAULT ''"
    "  ,endpoint TEXT NOT NULL DEFAULT ''"
    "  ,protocol_profile TEXT NOT NULL DEFAULT ''"
    "  ,modality TEXT NOT NULL DEFAULT 'text'"
    "  ,stream INTEGER NOT NULL DEFAULT 0"
    "  ,finish_code TEXT NOT NULL DEFAULT ''"
    "  ,error_code TEXT NOT NULL DEFAULT ''"
    "  ,cache_write_tokens INTEGER NOT NULL DEFAULT 0"
    "  ,reasoning_tokens INTEGER NOT NULL DEFAULT 0"
    "  ,queue_duration_ms REAL NOT NULL DEFAULT 0"
    "  ,swap_load_duration_ms REAL NOT NULL DEFAULT 0"
    "  ,first_token_duration_ms REAL NOT NULL DEFAULT 0"
    "  ,output_audio_seconds REAL NOT NULL DEFAULT 0"
    "  ,input_image_count INTEGER NOT NULL DEFAULT 0"
    "  ,output_image_count INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS swaps ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts INTEGER NOT NULL,"
    "  from_model TEXT NOT NULL,"
    "  to_model TEXT NOT NULL,"
    "  duration_ms REAL NOT NULL,"
    "  success INTEGER NOT NULL,"
    "  error TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_requests_ts ON requests(ts);"
    "CREATE INDEX IF NOT EXISTS idx_requests_model ON requests(model);"
    "CREATE INDEX IF NOT EXISTS idx_swaps_ts ON swaps(ts);";
  char* err = nullptr;
  if (sqlite3_exec(reinterpret_cast<sqlite3*>(db_), schema, nullptr, nullptr, &err) != SQLITE_OK) {
    if (err) sqlite3_free(err);
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    healthy_ = false;
    return;
  }
  // Existing databases predate billable speech units. SQLite has no
  // `ADD COLUMN IF NOT EXISTS`, so duplicate-column errors are intentionally
  // ignored while all other migration failures mark the database unhealthy.
  for (const char* migration : {
         "ALTER TABLE requests ADD COLUMN input_audio_seconds REAL NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN input_characters INTEGER NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN cached_prompt_tokens INTEGER NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN generation_duration_ms REAL NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN prompt_duration_ms REAL NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN prompt_tps REAL NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN resolved_model TEXT NOT NULL DEFAULT '';",
         "ALTER TABLE requests ADD COLUMN request_id TEXT NOT NULL DEFAULT '';",
         "ALTER TABLE requests ADD COLUMN principal_class TEXT NOT NULL DEFAULT '';",
         "ALTER TABLE requests ADD COLUMN endpoint TEXT NOT NULL DEFAULT '';",
         "ALTER TABLE requests ADD COLUMN protocol_profile TEXT NOT NULL DEFAULT '';",
         "ALTER TABLE requests ADD COLUMN modality TEXT NOT NULL DEFAULT 'text';",
         "ALTER TABLE requests ADD COLUMN stream INTEGER NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN finish_code TEXT NOT NULL DEFAULT '';",
         "ALTER TABLE requests ADD COLUMN error_code TEXT NOT NULL DEFAULT '';",
         "ALTER TABLE requests ADD COLUMN cache_write_tokens INTEGER NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN reasoning_tokens INTEGER NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN queue_duration_ms REAL NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN swap_load_duration_ms REAL NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN first_token_duration_ms REAL NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN output_audio_seconds REAL NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN input_image_count INTEGER NOT NULL DEFAULT 0;",
         "ALTER TABLE requests ADD COLUMN output_image_count INTEGER NOT NULL DEFAULT 0;",
       }) {
    char* migration_error = nullptr;
    if (sqlite3_exec(reinterpret_cast<sqlite3*>(db_), migration, nullptr, nullptr,
                     &migration_error) != SQLITE_OK) {
      const std::string message = migration_error ? migration_error : "";
      if (migration_error) sqlite3_free(migration_error);
      if (message.find("duplicate column name") == std::string::npos) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        healthy_ = false;
        return;
      }
    }
  }
  if (sqlite3_exec(db, "PRAGMA user_version=2; COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    healthy_ = false;
    return;
  }
  const char* request_sql =
    "INSERT INTO requests (ts, model, prompt_tokens, completion_tokens, duration_ms, tps, "
    "status_code, slot_id, input_audio_seconds, input_characters, cached_prompt_tokens, "
    "generation_duration_ms, prompt_duration_ms, prompt_tps, resolved_model, request_id, "
    "principal_class, endpoint, protocol_profile, modality, stream, finish_code, error_code, "
    "cache_write_tokens, reasoning_tokens, queue_duration_ms, swap_load_duration_ms, "
    "first_token_duration_ms, output_audio_seconds, input_image_count, output_image_count) "
    "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
  const char* swap_sql =
    "INSERT INTO swaps (ts, from_model, to_model, duration_ms, success, error) VALUES (?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(db, request_sql, -1,
          reinterpret_cast<sqlite3_stmt**>(&request_stmt_), nullptr) != SQLITE_OK ||
      sqlite3_prepare_v2(db, swap_sql, -1,
          reinterpret_cast<sqlite3_stmt**>(&swap_stmt_), nullptr) != SQLITE_OK) {
    healthy_ = false;
    return;
  }
  healthy_ = true;
}

void StatsDb::close() {
  std::lock_guard lk(mtx_);
  if (request_stmt_) {
    sqlite3_finalize(reinterpret_cast<sqlite3_stmt*>(request_stmt_));
    request_stmt_ = nullptr;
  }
  if (swap_stmt_) {
    sqlite3_finalize(reinterpret_cast<sqlite3_stmt*>(swap_stmt_));
    swap_stmt_ = nullptr;
  }
  if (db_) {
    sqlite3_close(reinterpret_cast<sqlite3*>(db_));
    db_ = nullptr;
  }
  healthy_ = false;
}

void StatsDb::record_request(const RequestRow& row) {
  if (!healthy_) return;
  std::lock_guard lk(mtx_);
  auto* stmt = reinterpret_cast<sqlite3_stmt*>(request_stmt_);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  const std::string model = row.model.empty() ? "unknown" : row.model;
  const std::string resolved_model = row.resolved_model.empty() ? model : row.resolved_model;
  const auto ts = row.timestamp_unix_ms > 0 ? row.timestamp_unix_ms : now_ms();
  const int prompt_tokens = std::max(0, row.prompt_tokens);
  const int cached_prompt_tokens = std::clamp(row.cached_prompt_tokens, 0, prompt_tokens);
  const int completion_tokens = std::max(0, row.completion_tokens);
  const double duration_ms = std::isfinite(row.duration_ms) ? std::max(0.0, row.duration_ms) : 0.0;
  const double tps = std::isfinite(row.tokens_per_second) ? std::max(0.0, row.tokens_per_second) : 0.0;
  const double generation_duration_ms = std::isfinite(row.generation_duration_ms)
      ? std::max(0.0, row.generation_duration_ms) : 0.0;
  const double prompt_duration_ms = std::isfinite(row.prompt_duration_ms)
      ? std::max(0.0, row.prompt_duration_ms) : 0.0;
  const double prompt_tps = std::isfinite(row.prompt_tokens_per_second)
      ? std::max(0.0, row.prompt_tokens_per_second) : 0.0;
  const double input_audio_seconds = std::isfinite(row.input_audio_seconds)
      ? std::max(0.0, row.input_audio_seconds) : 0.0;
  const auto input_characters = std::max<std::int64_t>(0, row.input_characters);
  sqlite3_bind_int64(stmt, 1, ts);
  sqlite3_bind_text(stmt, 2, model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, prompt_tokens);
  sqlite3_bind_int(stmt, 4, completion_tokens);
  sqlite3_bind_double(stmt, 5, duration_ms);
  sqlite3_bind_double(stmt, 6, tps);
  sqlite3_bind_int(stmt, 7, row.status_code);
  sqlite3_bind_int(stmt, 8, row.slot_id);
  sqlite3_bind_double(stmt, 9, input_audio_seconds);
  sqlite3_bind_int64(stmt, 10, input_characters);
  sqlite3_bind_int(stmt, 11, cached_prompt_tokens);
  sqlite3_bind_double(stmt, 12, generation_duration_ms);
  sqlite3_bind_double(stmt, 13, prompt_duration_ms);
  sqlite3_bind_double(stmt, 14, prompt_tps);
  sqlite3_bind_text(stmt, 15, resolved_model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 16, row.request_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 17, row.principal_class.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 18, row.endpoint.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 19, row.protocol_profile.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 20, row.modality.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 21, row.stream ? 1 : 0);
  sqlite3_bind_text(stmt, 22, row.finish_code.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 23, row.error_code.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 24, std::max(0, row.cache_write_tokens));
  sqlite3_bind_int(stmt, 25, std::max(0, row.reasoning_tokens));
  sqlite3_bind_double(stmt, 26, std::max(0.0, row.queue_duration_ms));
  sqlite3_bind_double(stmt, 27, std::max(0.0, row.swap_load_duration_ms));
  sqlite3_bind_double(stmt, 28, std::max(0.0, row.first_token_duration_ms));
  sqlite3_bind_double(stmt, 29, std::max(0.0, row.output_audio_seconds));
  sqlite3_bind_int(stmt, 30, std::max(0, row.input_image_count));
  sqlite3_bind_int(stmt, 31, std::max(0, row.output_image_count));
  if (sqlite3_step(stmt) != SQLITE_DONE) healthy_ = false;
}

void StatsDb::record_swap(const SwapRow& row) {
  if (!healthy_) return;
  std::lock_guard lk(mtx_);
  auto* stmt = reinterpret_cast<sqlite3_stmt*>(swap_stmt_);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  const auto ts = row.timestamp_unix_ms > 0 ? row.timestamp_unix_ms : now_ms();
  sqlite3_bind_int64(stmt, 1, ts);
  sqlite3_bind_text(stmt, 2, row.from_model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, row.to_model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 4, std::isfinite(row.duration_ms) ? std::max(0.0, row.duration_ms) : 0.0);
  sqlite3_bind_int(stmt, 5, row.success ? 1 : 0);
  sqlite3_bind_text(stmt, 6, row.error.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) healthy_ = false;
}

std::vector<RequestRow> StatsDb::recent_requests(
    int limit, const std::string& protocol_profile,
    const std::string& endpoint) const {
  std::vector<RequestRow> out;
  if (!healthy_) return out;
  std::lock_guard lk(mtx_);
  sqlite3_stmt* stmt = nullptr;
  std::string sql =
    "SELECT ts, model, prompt_tokens, completion_tokens, duration_ms, tps, status_code, slot_id, "
    "input_audio_seconds, input_characters, cached_prompt_tokens, generation_duration_ms, "
    "prompt_duration_ms, prompt_tps, resolved_model, request_id, principal_class, endpoint, "
    "protocol_profile, modality, stream, finish_code, error_code, cache_write_tokens, "
    "reasoning_tokens, queue_duration_ms, swap_load_duration_ms, first_token_duration_ms, "
    "output_audio_seconds, input_image_count, output_image_count FROM requests";
  if (!protocol_profile.empty() || !endpoint.empty()) {
    sql += " WHERE ";
    if (!protocol_profile.empty()) sql += "protocol_profile=?";
    if (!protocol_profile.empty() && !endpoint.empty()) sql += " AND ";
    if (!endpoint.empty()) sql += "endpoint=?";
  }
  sql += " ORDER BY id DESC LIMIT ?;";
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return out;
  int parameter = 1;
  if (!protocol_profile.empty()) {
    sqlite3_bind_text(stmt, parameter++, protocol_profile.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!endpoint.empty()) {
    sqlite3_bind_text(stmt, parameter++, endpoint.c_str(), -1, SQLITE_TRANSIENT);
  }
  sqlite3_bind_int(stmt, parameter, std::clamp(limit, 1, 10'000));
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    RequestRow r;
    r.timestamp_unix_ms    = sqlite3_column_int64(stmt, 0);
    r.model                = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    r.prompt_tokens        = sqlite3_column_int(stmt, 2);
    r.completion_tokens    = sqlite3_column_int(stmt, 3);
    r.duration_ms          = sqlite3_column_double(stmt, 4);
    r.tokens_per_second    = sqlite3_column_double(stmt, 5);
    r.status_code          = sqlite3_column_int(stmt, 6);
    r.slot_id              = sqlite3_column_int(stmt, 7);
    r.input_audio_seconds  = sqlite3_column_double(stmt, 8);
    r.input_characters     = sqlite3_column_int64(stmt, 9);
    r.cached_prompt_tokens = sqlite3_column_int(stmt, 10);
    r.generation_duration_ms = sqlite3_column_double(stmt, 11);
    r.prompt_duration_ms = sqlite3_column_double(stmt, 12);
    r.prompt_tokens_per_second = sqlite3_column_double(stmt, 13);
    r.resolved_model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
    r.request_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
    r.principal_class = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
    r.endpoint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 17));
    r.protocol_profile = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 18));
    r.modality = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 19));
    r.stream = sqlite3_column_int(stmt, 20) != 0;
    r.finish_code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 21));
    r.error_code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 22));
    r.cache_write_tokens = sqlite3_column_int(stmt, 23);
    r.reasoning_tokens = sqlite3_column_int(stmt, 24);
    r.queue_duration_ms = sqlite3_column_double(stmt, 25);
    r.swap_load_duration_ms = sqlite3_column_double(stmt, 26);
    r.first_token_duration_ms = sqlite3_column_double(stmt, 27);
    r.output_audio_seconds = sqlite3_column_double(stmt, 28);
    r.input_image_count = sqlite3_column_int(stmt, 29);
    r.output_image_count = sqlite3_column_int(stmt, 30);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<SwapRow> StatsDb::recent_swaps(int limit) const {
  std::vector<SwapRow> out;
  if (!healthy_) return out;
  std::lock_guard lk(mtx_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
    "SELECT ts, from_model, to_model, duration_ms, success, error FROM swaps "
    "ORDER BY id DESC LIMIT ?;";
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(stmt, 1, std::clamp(limit, 1, 10'000));
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    SwapRow r;
    r.timestamp_unix_ms = sqlite3_column_int64(stmt, 0);
    r.from_model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    r.to_model   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    r.duration_ms = sqlite3_column_double(stmt, 3);
    r.success = sqlite3_column_int(stmt, 4) != 0;
    r.error = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<ModelUsageRow> StatsDb::model_usage() const {
  std::vector<ModelUsageRow> out;
  if (!healthy_) return out;
  std::lock_guard lk(mtx_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
    "SELECT model, COUNT(*), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 THEN 1 ELSE 0 END),0), "
    "COALESCE(SUM(prompt_tokens),0), "
    "COALESCE(SUM(cached_prompt_tokens),0), COALESCE(SUM(completion_tokens),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN completion_tokens ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN prompt_tokens - cached_prompt_tokens ELSE 0 END),0), "
    "COALESCE(SUM(duration_ms),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN generation_duration_ms ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN prompt_duration_ms ELSE 0 END),0), "
    "COALESCE(MAX(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN completion_tokens * 1000.0 / generation_duration_ms ELSE 0 END),0), "
    "COALESCE(MAX(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN (prompt_tokens - cached_prompt_tokens) * 1000.0 / prompt_duration_ms ELSE 0 END),0), COALESCE(MAX(ts),0), "
    "COALESCE(SUM(input_audio_seconds),0), COALESCE(SUM(input_characters),0) "
    "FROM requests GROUP BY model ORDER BY MAX(ts) DESC;";
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    ModelUsageRow r;
    r.model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    r.requests = sqlite3_column_int64(stmt, 1);
    r.successful_requests = sqlite3_column_int64(stmt, 2);
    r.prompt_tokens = sqlite3_column_int64(stmt, 3);
    r.cached_prompt_tokens = sqlite3_column_int64(stmt, 4);
    r.completion_tokens = sqlite3_column_int64(stmt, 5);
    r.measured_completion_tokens = sqlite3_column_int64(stmt, 6);
    r.measured_prompt_tokens = sqlite3_column_int64(stmt, 7);
    r.total_duration_ms = sqlite3_column_double(stmt, 8);
    r.total_generation_duration_ms = sqlite3_column_double(stmt, 9);
    r.total_prompt_duration_ms = sqlite3_column_double(stmt, 10);
    r.peak_tokens_per_second = sqlite3_column_double(stmt, 11);
    r.peak_prompt_tokens_per_second = sqlite3_column_double(stmt, 12);
    r.last_timestamp_unix_ms = sqlite3_column_int64(stmt, 13);
    r.input_audio_seconds = sqlite3_column_double(stmt, 14);
    r.input_characters = sqlite3_column_int64(stmt, 15);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
  return out;
}

LifetimeTotals StatsDb::lifetime_totals() const {
  LifetimeTotals totals;
  if (!healthy_) return totals;
  std::lock_guard lk(mtx_);

  sqlite3_stmt* stmt = nullptr;
  const char* request_sql =
    "SELECT COUNT(*), COALESCE(SUM(prompt_tokens), 0), "
    "COALESCE(SUM(completion_tokens), 0), COALESCE(SUM(duration_ms), 0.0) "
    "FROM requests;";
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), request_sql, -1,
                         &stmt, nullptr) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW) {
    totals.requests = sqlite3_column_int64(stmt, 0);
    totals.prompt_tokens = sqlite3_column_int64(stmt, 1);
    totals.completion_tokens = sqlite3_column_int64(stmt, 2);
    totals.total_duration_ms = sqlite3_column_double(stmt, 3);
  }
  sqlite3_finalize(stmt);

  stmt = nullptr;
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_),
                         "SELECT COUNT(*) FROM swaps;", -1,
                         &stmt, nullptr) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW) {
    totals.swaps = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return totals;
}

std::vector<UsageBucketRow> StatsDb::monthly_usage(int months) const {
  std::vector<UsageBucketRow> out;
  if (!healthy_) return out;
  std::lock_guard lk(mtx_);
  sqlite3_stmt* stmt = nullptr;
  const char* all_time_sql =
    "SELECT strftime('%Y-%m', ts / 1000, 'unixepoch') AS bucket, model, "
    "COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(cached_prompt_tokens),0), "
    "COALESCE(SUM(completion_tokens),0), COALESCE(SUM(prompt_tokens + completion_tokens),0), COUNT(*), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 THEN 1 ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN completion_tokens ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN prompt_tokens - cached_prompt_tokens ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN generation_duration_ms ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN prompt_duration_ms ELSE 0 END),0), "
    "COALESCE(MAX(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN completion_tokens * 1000.0 / generation_duration_ms ELSE 0 END),0), "
    "COALESCE(MAX(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN (prompt_tokens - cached_prompt_tokens) * 1000.0 / prompt_duration_ms ELSE 0 END),0), "
    "COALESCE(SUM(input_audio_seconds),0), COALESCE(SUM(input_characters),0) "
    "FROM requests "
    "GROUP BY bucket, model ORDER BY bucket ASC, model ASC;";
  const char* limited_sql =
    "SELECT strftime('%Y-%m', ts / 1000, 'unixepoch') AS bucket, model, "
    "COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(cached_prompt_tokens),0), "
    "COALESCE(SUM(completion_tokens),0), COALESCE(SUM(prompt_tokens + completion_tokens),0), COUNT(*), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 THEN 1 ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN completion_tokens ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN prompt_tokens - cached_prompt_tokens ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN generation_duration_ms ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN prompt_duration_ms ELSE 0 END),0), "
    "COALESCE(MAX(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN completion_tokens * 1000.0 / generation_duration_ms ELSE 0 END),0), "
    "COALESCE(MAX(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN (prompt_tokens - cached_prompt_tokens) * 1000.0 / prompt_duration_ms ELSE 0 END),0), "
    "COALESCE(SUM(input_audio_seconds),0), COALESCE(SUM(input_characters),0) "
    "FROM requests "
    "WHERE ts >= ((strftime('%s','now','start of month', ?) * 1000)) "
    "GROUP BY bucket, model ORDER BY bucket ASC, model ASC;";
  const char* sql = months <= 0 ? all_time_sql : limited_sql;
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  std::string modifier;
  if (months > 0) {
    modifier = "-" + std::to_string(months - 1) + " months";
    sqlite3_bind_text(stmt, 1, modifier.c_str(), -1, SQLITE_TRANSIENT);
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    UsageBucketRow r;
    r.bucket = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    r.model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    r.prompt_tokens = sqlite3_column_int64(stmt, 2);
    r.cached_prompt_tokens = sqlite3_column_int64(stmt, 3);
    r.completion_tokens = sqlite3_column_int64(stmt, 4);
    r.total_tokens = sqlite3_column_int64(stmt, 5);
    r.requests = sqlite3_column_int64(stmt, 6);
    r.successful_requests = sqlite3_column_int64(stmt, 7);
    r.measured_completion_tokens = sqlite3_column_int64(stmt, 8);
    r.measured_prompt_tokens = sqlite3_column_int64(stmt, 9);
    r.generation_duration_ms = sqlite3_column_double(stmt, 10);
    r.prompt_duration_ms = sqlite3_column_double(stmt, 11);
    r.peak_tokens_per_second = sqlite3_column_double(stmt, 12);
    r.peak_prompt_tokens_per_second = sqlite3_column_double(stmt, 13);
    r.input_audio_seconds = sqlite3_column_double(stmt, 14);
    r.input_characters = sqlite3_column_int64(stmt, 15);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<UsageBucketRow> StatsDb::daily_usage(int days) const {
  const auto today_ms = now_ms() / 86'400'000 * 86'400'000;
  return bucketed_usage(
      "%Y-%m-%d",
      days <= 0 ? 0 : today_ms -
          static_cast<std::int64_t>(std::max(0, days - 1)) * 86'400'000);
}

std::vector<UsageBucketRow> StatsDb::hourly_usage(int hours) const {
  return bucketed_usage("%Y-%m-%dT%H", now_ms() - static_cast<std::int64_t>(hours) * 3'600'000);
}

std::vector<UsageBucketRow> StatsDb::bucketed_usage(const char* fmt, std::int64_t since_ms) const {
  std::vector<UsageBucketRow> out;
  if (!healthy_) return out;
  std::lock_guard lk(mtx_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
    "SELECT strftime(?, ts / 1000, 'unixepoch') AS bucket, model, "
    "COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(cached_prompt_tokens),0), "
    "COALESCE(SUM(completion_tokens),0), COALESCE(SUM(prompt_tokens + completion_tokens),0), COUNT(*), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 THEN 1 ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN completion_tokens ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN prompt_tokens - cached_prompt_tokens ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN generation_duration_ms ELSE 0 END),0), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN prompt_duration_ms ELSE 0 END),0), "
    "COALESCE(MAX(CASE WHEN status_code >= 200 AND status_code < 300 AND generation_duration_ms > 0 AND completion_tokens > 0 THEN completion_tokens * 1000.0 / generation_duration_ms ELSE 0 END),0), "
    "COALESCE(MAX(CASE WHEN status_code >= 200 AND status_code < 300 AND prompt_duration_ms > 0 AND prompt_tokens > cached_prompt_tokens THEN (prompt_tokens - cached_prompt_tokens) * 1000.0 / prompt_duration_ms ELSE 0 END),0), "
    "COALESCE(SUM(input_audio_seconds),0), COALESCE(SUM(input_characters),0) "
    "FROM requests WHERE ts >= ? "
    "GROUP BY bucket, model ORDER BY bucket ASC, model ASC;";
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(stmt, 1, fmt, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, since_ms);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    UsageBucketRow r;
    r.bucket = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    r.model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    r.prompt_tokens = sqlite3_column_int64(stmt, 2);
    r.cached_prompt_tokens = sqlite3_column_int64(stmt, 3);
    r.completion_tokens = sqlite3_column_int64(stmt, 4);
    r.total_tokens = sqlite3_column_int64(stmt, 5);
    r.requests = sqlite3_column_int64(stmt, 6);
    r.successful_requests = sqlite3_column_int64(stmt, 7);
    r.measured_completion_tokens = sqlite3_column_int64(stmt, 8);
    r.measured_prompt_tokens = sqlite3_column_int64(stmt, 9);
    r.generation_duration_ms = sqlite3_column_double(stmt, 10);
    r.prompt_duration_ms = sqlite3_column_double(stmt, 11);
    r.peak_tokens_per_second = sqlite3_column_double(stmt, 12);
    r.peak_prompt_tokens_per_second = sqlite3_column_double(stmt, 13);
    r.input_audio_seconds = sqlite3_column_double(stmt, 14);
    r.input_characters = sqlite3_column_int64(stmt, 15);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
  return out;
}

}
