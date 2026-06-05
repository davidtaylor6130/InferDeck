#include <catch2/catch_test_macros.hpp>

#include "observability/metrics.hpp"

using namespace inferdeck::observability;

TEST_CASE("Metrics starts at zero", "[observability][metrics]") {
  Metrics m;
  REQUIRE(m.total_requests() == 0);
  REQUIRE(m.total_swaps() == 0);
  REQUIRE(m.total_prompt_tokens() == 0);
  REQUIRE(m.total_completion_tokens() == 0);
  REQUIRE(m.total_duration_ms() == 0.0);
  REQUIRE(m.avg_tokens_per_second() == 0.0);
  REQUIRE(m.last_gpu_util_pct() == 0.0);
  auto s = m.snapshot_for("nope");
  REQUIRE(s.requests == 0);
}

TEST_CASE("Metrics record_request accumulates", "[observability][metrics]") {
  Metrics m;
  RequestRecord r1{1000, "qwen3.6-27b", 10, 20, 500.0, 40.0, 200, 0};
  RequestRecord r2{1500, "qwen3.6-27b", 30, 40, 800.0, 50.0, 200, 1};
  RequestRecord r3{2000, "qwen3-coder-next", 5, 100, 2000.0, 50.0, 200, 0};
  m.record_request(r1);
  m.record_request(r2);
  m.record_request(r3);
  REQUIRE(m.total_requests() == 3);
  REQUIRE(m.total_prompt_tokens() == 45);
  REQUIRE(m.total_completion_tokens() == 160);
  REQUIRE(m.total_duration_ms() == Approx(3300.0));
  REQUIRE(m.avg_tokens_per_second() == Approx(160.0 / 3.3).margin(0.01));

  auto s27 = m.snapshot_for("qwen3.6-27b");
  REQUIRE(s27.requests == 2);
  REQUIRE(s27.prompt_tokens == 40);
  REQUIRE(s27.completion_tokens == 60);
  REQUIRE(s27.peak_tokens_per_second() == 50.0);
  REQUIRE(s27.last_tokens_per_second() == 50.0);
  REQUIRE(s27.last_timestamp_unix_ms == 1500);

  auto scn = m.snapshot_for("qwen3-coder-next");
  REQUIRE(scn.requests == 1);
  REQUIRE(scn.completion_tokens == 100);
}

TEST_CASE("Metrics record_swap", "[observability][metrics]") {
  Metrics m;
  SwapRecord s{1, "a", "b", 1500.0, true, ""};
  m.record_swap(s);
  m.record_swap(s);
  REQUIRE(m.total_swaps() == 2);
}

TEST_CASE("Metrics record_gpu_sample updates last_*", "[observability][metrics]") {
  Metrics m;
  m.record_gpu_sample(5000, 75.0, 16384.0, 62.5, 240.0);
  REQUIRE(m.last_gpu_sample_unix_ms() == 5000);
  REQUIRE(m.last_gpu_util_pct() == 75.0);
  REQUIRE(m.last_gpu_vram_mb()  == 16384.0);
  REQUIRE(m.last_gpu_temp_c()   == 62.5);
  REQUIRE(m.last_gpu_power_w()  == 240.0);
}

TEST_CASE("Metrics reset clears all counters", "[observability][metrics]") {
  Metrics m;
  m.record_request({1, "x", 1, 1, 1.0, 1.0, 200, 0});
  m.record_swap({1, "a", "b", 1.0, true, ""});
  m.record_gpu_sample(1, 1.0, 1.0, 1.0, 1.0);
  m.reset();
  REQUIRE(m.total_requests() == 0);
  REQUIRE(m.total_swaps() == 0);
  REQUIRE(m.total_prompt_tokens() == 0);
  REQUIRE(m.last_gpu_util_pct() == 0.0);
}
