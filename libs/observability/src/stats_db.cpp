#include "observability/stats_db.hpp"

#include <sqlite3.h>
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

}

StatsDb::StatsDb(const std::string& path) : path_(path) { open(); }
StatsDb::~StatsDb() { close(); }

void StatsDb::open() {
  std::lock_guard lk(mtx_);
  if (sqlite3_open(path_.c_str(), reinterpret_cast<sqlite3**>(&db_)) != SQLITE_OK) {
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
  const char* sql =
    "INSERT INTO requests (ts, model, prompt_tokens, completion_tokens, "
    "  duration_ms, tps, status_code, slot_id) VALUES (?,?,?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) return;
  sqlite3_bind_int64(stmt, 1, row.timestamp_unix_ms);
  sqlite3_bind_text(stmt, 2, row.model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, row.prompt_tokens);
  sqlite3_bind_int(stmt, 4, row.completion_tokens);
  sqlite3_bind_double(stmt, 5, row.duration_ms);
  sqlite3_bind_double(stmt, 6, row.tokens_per_second);
  sqlite3_bind_int(stmt, 7, row.status_code);
  sqlite3_bind_int(stmt, 8, row.slot_id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void StatsDb::record_swap(const SwapRow& row) {
  if (!healthy_) return;
  std::lock_guard lk(mtx_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
    "INSERT INTO swaps (ts, from_model, to_model, duration_ms, success, error) "
    "VALUES (?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(reinterpret_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr) != SQLITE_OK) return;
  sqlite3_bind_int64(stmt, 1, row.timestamp_unix_ms);
  sqlite3_bind_text(stmt, 2, row.from_model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, row.to_model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 4, row.duration_ms);
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

}
