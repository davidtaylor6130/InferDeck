#include "observability/stats_db.hpp"

#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace inferdeck::observability {

namespace {

void throw_on_error(int rc, sqlite3* db, const char* what) {
  if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
    std::string msg = std::string("sqlite: ") + what + ": " + (db ? sqlite3_errmsg(db) : "?");
    throw std::runtime_error(msg);
  }
}

std::string expand_path(std::string path) {
  if (path.empty()) return path;
  if (path == ":memory:") return path;
  if (path[0] == '~') {
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    if (home) {
      if (path.size() == 1) return home;
      if (path[1] == '/' || path[1] == '\\') {
        return std::string(home) + path.substr(1);
      }
    }
  }
  return path;
}

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}

StatsDb::StatsDb(const std::string& path) : path_(expand_path(path)) { open(); }
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
  char* pragma_err = nullptr;
  if (sqlite3_exec(reinterpret_cast<sqlite3*>(db_),
                   "PRAGMA journal_mode=WAL;"
                   "PRAGMA synchronous=FULL;"
                   "PRAGMA temp_store=MEMORY;",
                   nullptr, nullptr, &pragma_err) != SQLITE_OK) {
    if (pragma_err) sqlite3_free(pragma_err);
    healthy_ = false;
    return;
  }
  const char* schema =
    "CREATE TABLE IF NOT EXISTS requests ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts INTEGER NOT NULL,"
    "  model TEXT NOT NULL,"
    "  prompt_tokens INTEGER NOT NULL,"
    "  completion_tokens INTEGER NOT NULL,"
    "  duration_ms REAL NOT NULL,"
    "  tps REAL NOT NULL,"
    "  status_code INTEGER NOT NULL,"
    "  slot_id INTEGER NOT NULL"
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
    healthy_ = false;
    return;
  }
  healthy_ = true;
}

void StatsDb::close() {
  std::lock_guard lk(mtx_);
  if (db_) {
    sqlite3_close(reinterpret_cast<sqlite3*>(db_));
    db_ = nullptr;
  }
  healthy_ = false;
}

void StatsDb::record_request(const RequestRow& row) {
  if (!healthy_) return;
  std::lock_guard lk(mtx_);
  sqlite3_stmt* stmt = nullptr;
  const std::string model = row.model.empty() ? "unknown" : row.model;
  const auto ts = row.timestamp_unix_ms > 0 ? row.timestamp_unix_ms : now_ms();
  const int prompt_tokens = std::max(0, row.prompt_tokens);
  const int completion_tokens = std::max(0, row.completion_tokens);
  const double duration_ms = std::max(0.0, row.duration_ms);
  const double tps = std::max(0.0, row.tokens_per_second);
  const char* sql =
    "INSERT INTO requests (ts, model, prompt_tokens, completion_tokens, "
    "  duration_ms, tps, status_code, slot_id) VALUES (?,?,?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) return;
  sqlite3_bind_int64(stmt, 1, ts);
  sqlite3_bind_text(stmt, 2, model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, prompt_tokens);
  sqlite3_bind_int(stmt, 4, completion_tokens);
  sqlite3_bind_double(stmt, 5, duration_ms);
  sqlite3_bind_double(stmt, 6, tps);
  sqlite3_bind_int(stmt, 7, row.status_code);
  sqlite3_bind_int(stmt, 8, row.slot_id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void StatsDb::record_swap(const SwapRow& row) {
  if (!healthy_) return;
  std::lock_guard lk(mtx_);
  sqlite3_stmt* stmt = nullptr;
  const auto ts = row.timestamp_unix_ms > 0 ? row.timestamp_unix_ms : now_ms();
  const char* sql =
    "INSERT INTO swaps (ts, from_model, to_model, duration_ms, success, error) "
    "VALUES (?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) return;
  sqlite3_bind_int64(stmt, 1, ts);
  sqlite3_bind_text(stmt, 2, row.from_model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, row.to_model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 4, std::max(0.0, row.duration_ms));
  sqlite3_bind_int(stmt, 5, row.success ? 1 : 0);
  sqlite3_bind_text(stmt, 6, row.error.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

std::vector<RequestRow> StatsDb::recent_requests(int limit) const {
  std::vector<RequestRow> out;
  if (!healthy_) return out;
  std::lock_guard lk(mtx_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
    "SELECT ts, model, prompt_tokens, completion_tokens, duration_ms, tps, status_code, slot_id "
    "FROM requests ORDER BY id DESC LIMIT ?;";
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(stmt, 1, limit);
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
  sqlite3_bind_int(stmt, 1, limit);
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
    "COALESCE(SUM(completion_tokens),0), COALESCE(SUM(duration_ms),0), "
    "COALESCE(MAX(tps),0), COALESCE(MAX(ts),0) "
    "FROM requests GROUP BY model ORDER BY MAX(ts) DESC;";
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    ModelUsageRow r;
    r.model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    r.requests = sqlite3_column_int64(stmt, 1);
    r.successful_requests = sqlite3_column_int64(stmt, 2);
    r.prompt_tokens = sqlite3_column_int64(stmt, 3);
    r.completion_tokens = sqlite3_column_int64(stmt, 4);
    r.total_duration_ms = sqlite3_column_double(stmt, 5);
    r.peak_tokens_per_second = sqlite3_column_double(stmt, 6);
    r.last_timestamp_unix_ms = sqlite3_column_int64(stmt, 7);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<UsageBucketRow> StatsDb::monthly_usage(int months) const {
  std::vector<UsageBucketRow> out;
  if (!healthy_) return out;
  std::lock_guard lk(mtx_);
  sqlite3_stmt* stmt = nullptr;
  const char* all_time_sql =
    "SELECT strftime('%Y-%m', ts / 1000, 'unixepoch') AS bucket, model, "
    "COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(completion_tokens),0), "
    "COALESCE(SUM(prompt_tokens + completion_tokens),0), COUNT(*), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 THEN 1 ELSE 0 END),0) "
    "FROM requests "
    "GROUP BY bucket, model ORDER BY bucket ASC, model ASC;";
  const char* limited_sql =
    "SELECT strftime('%Y-%m', ts / 1000, 'unixepoch') AS bucket, model, "
    "COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(completion_tokens),0), "
    "COALESCE(SUM(prompt_tokens + completion_tokens),0), COUNT(*), "
    "COALESCE(SUM(CASE WHEN status_code >= 200 AND status_code < 300 THEN 1 ELSE 0 END),0) "
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
    r.completion_tokens = sqlite3_column_int64(stmt, 3);
    r.total_tokens = sqlite3_column_int64(stmt, 4);
    r.requests = sqlite3_column_int64(stmt, 5);
    r.successful_requests = sqlite3_column_int64(stmt, 6);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
  return out;
}

}
