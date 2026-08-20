#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <algorithm>
#include <limits>
#include <thread>
#include <sqlite3.h>

#include "observability/stats_db.hpp"
#include "test_helpers.hpp"

using namespace inferdeck::observability;
using namespace std::chrono_literals;

TEST_CASE("StatsDb: opens in-memory path and reports healthy", "[observability][stats]") {
  const auto dir = test_helpers::make_temp_dir("statsdb");
  const auto path = (dir / "stats.db").string();
  StatsDb db(path);
  REQUIRE(db.healthy());
  REQUIRE(db.path() == path);
}

TEST_CASE("StatsDb: record_request persists and round-trips", "[observability][stats]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_req");
  const auto path = (dir / "stats.db").string();
  StatsDb db(path);
  REQUIRE(db.healthy());
  db.record_request({1000, "qwen3.6-27b", 10, 20, 500.0, 40.0, 200, 0});
  db.record_request({1500, "qwen3.6-27b", 30, 40, 800.0, 50.0, 200, 1});
  db.record_request({2000, "qwen3-coder-next", 5, 100, 2000.0, 50.0, 200, 0});
  auto rows = db.recent_requests(10);
  REQUIRE(rows.size() == 3);
  REQUIRE(rows[0].model == "qwen3-coder-next");
  REQUIRE(rows[0].prompt_tokens == 5);
  REQUIRE(rows[0].completion_tokens == 100);
  REQUIRE(rows[1].model == "qwen3.6-27b");
  REQUIRE(rows[1].prompt_tokens == 30);
  REQUIRE(rows[2].model == "qwen3.6-27b");
  REQUIRE(rows[2].prompt_tokens == 10);
}

TEST_CASE("StatsDb: model usage aggregates survive reopen", "[observability][stats]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_usage");
  const auto path = (dir / "stats.db").string();
  {
    StatsDb db(path);
    REQUIRE(db.healthy());
    db.record_request({1000, "qwen3.6-27b", 10, 20, 500.0, 40.0, 200, 0});
    db.record_request({1500, "qwen3.6-27b", 30, 40, 800.0, 50.0, 200, 1});
    db.record_request({1750, "qwen3.6-27b", 0, 0, 10.0, 0.0, 500, 1});
    db.record_request({2000, "qwen3-coder-next", 5, 100, 2000.0, 50.0, 200, 0});
  }
  StatsDb reopened(path);
  REQUIRE(reopened.healthy());
  auto usage = reopened.model_usage();
  REQUIRE(usage.size() == 2);
  auto qwen = std::find_if(usage.begin(), usage.end(), [](const ModelUsageRow& row) {
    return row.model == "qwen3.6-27b";
  });
  REQUIRE(qwen != usage.end());
  REQUIRE(qwen->requests == 3);
  REQUIRE(qwen->successful_requests == 2);
  REQUIRE(qwen->prompt_tokens == 40);
  REQUIRE(qwen->completion_tokens == 60);
  REQUIRE(qwen->measured_completion_tokens == 0);
  REQUIRE(qwen->measured_prompt_tokens == 0);
  REQUIRE(qwen->peak_tokens_per_second == 0.0);
}

TEST_CASE("StatsDb: speech billing units persist and aggregate across restarts",
          "[observability][stats][speech][restart]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_speech");
  const auto path = (dir / "stats.db").string();
  {
    StatsDb db(path);
    REQUIRE(db.healthy());
    db.record_request({1000, "parakeet-tdt-0.6b-v3", 0, 0, 900.0, 0.0,
                       200, 0, 42.5, 0});
    db.record_request({1500, "supertonic-3", 0, 0, 400.0, 0.0,
                       200, 0, 0.0, 12'345});
  }

  StatsDb reopened(path);
  REQUIRE(reopened.healthy());
  const auto requests = reopened.recent_requests(10);
  REQUIRE(requests.size() == 2);
  CHECK(requests[0].input_characters == 12'345);
  CHECK(requests[1].input_audio_seconds == Catch::Approx(42.5));

  const auto usage = reopened.model_usage();
  const auto transcription = std::find_if(
      usage.begin(), usage.end(),
      [](const ModelUsageRow& row) { return row.model == "parakeet-tdt-0.6b-v3"; });
  const auto speech = std::find_if(
      usage.begin(), usage.end(),
      [](const ModelUsageRow& row) { return row.model == "supertonic-3"; });
  REQUIRE(transcription != usage.end());
  REQUIRE(speech != usage.end());
  CHECK(transcription->input_audio_seconds == Catch::Approx(42.5));
  CHECK(speech->input_characters == 12'345);
}

TEST_CASE("StatsDb: existing token ledger is migrated without losing history",
          "[observability][stats][migration]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_migrate_speech");
  const auto path = (dir / "stats.db").string();
  sqlite3* legacy = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &legacy) == SQLITE_OK);
  const char* legacy_schema =
      "CREATE TABLE requests ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, model TEXT NOT NULL,"
      "prompt_tokens INTEGER NOT NULL, completion_tokens INTEGER NOT NULL,"
      "duration_ms REAL NOT NULL, tps REAL NOT NULL, status_code INTEGER NOT NULL,"
      "slot_id INTEGER NOT NULL);"
      "INSERT INTO requests (ts, model, prompt_tokens, completion_tokens, duration_ms,"
      "tps, status_code, slot_id) VALUES (1000, 'historic-llm', 100, 25, 500, 500, 200, 0);";
  char* error = nullptr;
  REQUIRE(sqlite3_exec(legacy, legacy_schema, nullptr, nullptr, &error) == SQLITE_OK);
  sqlite3_close(legacy);

  StatsDb migrated(path);
  REQUIRE(migrated.healthy());
  const auto rows = migrated.recent_requests(10);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].model == "historic-llm");
  CHECK(rows[0].prompt_tokens == 100);
  CHECK(rows[0].completion_tokens == 25);
  CHECK(rows[0].input_audio_seconds == 0.0);
  CHECK(rows[0].input_characters == 0);

  migrated.record_request({2000, "historic-llm", 80, 40, 700.0, 80.0, 200, 0,
                           0.0, 0, 20, 500.0, 200.0, 300.0});
  migrated.record_request({2500, "historic-llm", 10, 10, 20.0, 500.0, 500, 0,
                           0.0, 0, 0, 10.0, 10.0, 1000.0});
  const auto usage = migrated.model_usage();
  REQUIRE(usage.size() == 1);
  CHECK(usage[0].prompt_tokens == 190);
  CHECK(usage[0].completion_tokens == 75);
  CHECK(usage[0].measured_completion_tokens == 40);
  CHECK(usage[0].measured_prompt_tokens == 60);
  CHECK(usage[0].total_generation_duration_ms == Catch::Approx(500.0));
  CHECK(usage[0].total_prompt_duration_ms == Catch::Approx(200.0));
  CHECK(usage[0].peak_tokens_per_second == Catch::Approx(80.0));
  CHECK(usage[0].peak_prompt_tokens_per_second == Catch::Approx(300.0));

  const auto monthly = migrated.monthly_usage();
  REQUIRE(monthly.size() == 1);
  CHECK(monthly[0].measured_completion_tokens == 40);
  CHECK(monthly[0].measured_prompt_tokens == 60);
  CHECK(monthly[0].peak_tokens_per_second == Catch::Approx(80.0));
}

TEST_CASE("StatsDb: lifetime totals survive reopen for metrics restoration",
          "[observability][stats][restart]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_lifetime");
  const auto path = (dir / "stats.db").string();
  {
    StatsDb db(path);
    REQUIRE(db.healthy());
    db.record_request({1000, "qwen3.6-27b", 10, 20, 500.0, 40.0, 200, 0});
    db.record_request({1500, "qwen3.6-27b", 30, 40, 800.0, 50.0, 500, 1});
    db.record_swap({1600, "a", "b", 100.0, true, ""});
  }

  StatsDb reopened(path);
  REQUIRE(reopened.healthy());
  const auto totals = reopened.lifetime_totals();
  REQUIRE(totals.requests == 2);
  REQUIRE(totals.swaps == 1);
  REQUIRE(totals.prompt_tokens == 40);
  REQUIRE(totals.completion_tokens == 60);
  REQUIRE(totals.total_duration_ms == Catch::Approx(1300.0));
}

TEST_CASE("StatsDb: monthly usage buckets successful counts", "[observability][stats]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_monthly");
  const auto path = (dir / "stats.db").string();
  StatsDb db(path);
  REQUIRE(db.healthy());
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  db.record_request({now, "qwen3.6-27b", 10, 20, 500.0, 40.0, 200, 0});
  db.record_request({now + 1, "qwen3.6-27b", 30, 40, 800.0, 50.0, 500, 1});
  auto rows = db.monthly_usage(1);
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0].model == "qwen3.6-27b");
  REQUIRE(rows[0].prompt_tokens == 40);
  REQUIRE(rows[0].completion_tokens == 60);
  REQUIRE(rows[0].total_tokens == 100);
  REQUIRE(rows[0].requests == 2);
  REQUIRE(rows[0].successful_requests == 1);
}

TEST_CASE("StatsDb: monthly usage defaults to all-time buckets", "[observability][stats]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_alltime");
  const auto path = (dir / "stats.db").string();
  StatsDb db(path);
  REQUIRE(db.healthy());
  db.record_request({1704067200000LL, "old-model", 100, 50, 0.0, 0.0, 200, -1});
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  db.record_request({now, "new-model", 10, 5, 0.0, 0.0, 200, -1});
  auto rows = db.monthly_usage();
  REQUIRE(rows.size() == 2);
  auto old_row = std::find_if(rows.begin(), rows.end(), [](const UsageBucketRow& row) {
    return row.model == "old-model";
  });
  REQUIRE(old_row != rows.end());
  REQUIRE(old_row->bucket == "2024-01");
  REQUIRE(old_row->total_tokens == 150);
}

TEST_CASE("StatsDb: zero-day daily usage returns all-time resolution",
          "[observability][stats][usage-range]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_daily_alltime");
  const auto path = (dir / "stats.db").string();
  StatsDb db(path);
  REQUIRE(db.healthy());
  db.record_request({1704067200000LL, "old-model", 100, 50, 0.0, 0.0, 200, -1});
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  db.record_request({now, "new-model", 10, 5, 0.0, 0.0, 200, -1});
  const auto rows = db.daily_usage(0);
  REQUIRE(rows.size() == 2);
  CHECK(std::any_of(rows.begin(), rows.end(), [](const UsageBucketRow& row) {
    return row.model == "old-model" && row.bucket == "2024-01-01";
  }));
}

TEST_CASE("StatsDb: billing buckets use UTC across daylight-saving boundaries",
          "[observability][stats][usage-range][utc]") {
  StatsDb db(":memory:");
  REQUIRE(db.healthy());
  db.record_request({1787009400000LL, "boundary-model", 100, 50,
                     0.0, 0.0, 200, -1});
  const auto daily = db.daily_usage(0);
  const auto hourly = db.hourly_usage(24 * 365 * 100);
  REQUIRE(daily.size() == 1);
  REQUIRE(hourly.size() == 1);
  CHECK(daily[0].bucket == "2026-08-17");
  CHECK(hourly[0].bucket == "2026-08-17T23");
}

TEST_CASE("StatsDb: request rows are sanitized before persistence", "[observability][stats]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_sanitize");
  const auto path = (dir / "stats.db").string();
  StatsDb db(path);
  REQUIRE(db.healthy());
  db.record_request({0, "", -10, -20, -30.0, -40.0, 200, -1});
  db.record_request({0, "nonfinite", 1, 1,
                     std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::infinity(), 200, -1,
                     std::numeric_limits<double>::quiet_NaN(), -20});
  auto rows = db.recent_requests(2);
  REQUIRE(rows.size() == 2);
  REQUIRE(rows[0].timestamp_unix_ms > 0);
  REQUIRE(rows[0].model == "nonfinite");
  REQUIRE(rows[0].duration_ms == 0.0);
  REQUIRE(rows[0].tokens_per_second == 0.0);
  REQUIRE(rows[0].input_audio_seconds == 0.0);
  REQUIRE(rows[0].input_characters == 0);
  REQUIRE(rows[1].model == "unknown");
  REQUIRE(rows[1].prompt_tokens == 0);
  REQUIRE(rows[1].completion_tokens == 0);
}

TEST_CASE("StatsDb: record_swap persists and round-trips", "[observability][stats]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_swap");
  const auto path = (dir / "stats.db").string();
  StatsDb db(path);
  REQUIRE(db.healthy());
  db.record_swap({1, "qwen3.6-27b", "qwen3-coder-next", 1500.0, true, ""});
  db.record_swap({2, "qwen3-coder-next", "qwen3.6-27b", 0.0, false, "model_not_registered"});
  auto rows = db.recent_swaps(10);
  REQUIRE(rows.size() == 2);
  REQUIRE(rows[0].from_model == "qwen3-coder-next");
  REQUIRE_FALSE(rows[0].success);
  REQUIRE(rows[0].error == "model_not_registered");
  REQUIRE(rows[1].from_model == "qwen3.6-27b");
  REQUIRE(rows[1].to_model == "qwen3-coder-next");
  REQUIRE(rows[1].success);
  REQUIRE(rows[1].duration_ms == 1500.0);
}

TEST_CASE("StatsDb: recent_requests honors limit", "[observability][stats]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_lim");
  const auto path = (dir / "stats.db").string();
  StatsDb db(path);
  REQUIRE(db.healthy());
  for (int i = 0; i < 25; ++i) {
    db.record_request({i, "m", 1, 1, 1.0, 1.0, 200, 0});
  }
  REQUIRE(db.recent_requests(5).size() == 5);
  REQUIRE(db.recent_requests(100).size() == 25);
}

TEST_CASE("StatsDb: creates missing parent directories", "[observability][stats]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_parent");
  const auto path = (dir / "nested" / "deeper" / "stats.db").string();
  StatsDb db(path);
  REQUIRE(db.healthy());
  db.record_request({1, "x", 1, 1, 1.0, 1.0, 200, 0});
  REQUIRE(db.recent_requests(10).size() == 1);
}

TEST_CASE("StatsDb: close on destruction", "[observability][stats]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_close");
  const auto path = (dir / "stats.db").string();
  {
    StatsDb db(path);
    REQUIRE(db.healthy());
  }
  StatsDb db2(path);
  REQUIRE(db2.healthy());
  db2.record_request({1, "x", 1, 1, 1.0, 1.0, 200, 0});
  REQUIRE(db2.recent_requests(10).size() == 1);
}
