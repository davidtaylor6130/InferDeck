#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <algorithm>
#include <thread>

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
  REQUIRE(qwen->peak_tokens_per_second == 50.0);
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

TEST_CASE("StatsDb: request rows are sanitized before persistence", "[observability][stats]") {
  const auto dir = test_helpers::make_temp_dir("statsdb_sanitize");
  const auto path = (dir / "stats.db").string();
  StatsDb db(path);
  REQUIRE(db.healthy());
  db.record_request({0, "", -10, -20, -30.0, -40.0, 200, -1});
  auto rows = db.recent_requests(1);
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0].timestamp_unix_ms > 0);
  REQUIRE(rows[0].model == "unknown");
  REQUIRE(rows[0].prompt_tokens == 0);
  REQUIRE(rows[0].completion_tokens == 0);
  REQUIRE(rows[0].duration_ms == 0.0);
  REQUIRE(rows[0].tokens_per_second == 0.0);
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
