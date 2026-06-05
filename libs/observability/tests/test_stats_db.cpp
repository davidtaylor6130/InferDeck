#include <catch2/catch_test_macros.hpp>

#include <chrono>
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

TEST_CASE("StatsDb: open on bad path is unhealthy but does not throw", "[observability][stats]") {
  StatsDb db("C:/this/path/should/never/exist/zxq/foo.db");
  REQUIRE_FALSE(db.healthy());
  db.record_request({1, "x", 1, 1, 1.0, 1.0, 200, 0});
  REQUIRE(db.recent_requests(10).empty());
  db.record_swap({1, "a", "b", 1.0, true, ""});
  REQUIRE(db.recent_swaps(10).empty());
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
