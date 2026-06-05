#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include "observability/gpu_telemetry.hpp"
#include "test_helpers.hpp"

using namespace inferdeck::observability;
using namespace std::chrono_literals;

TEST_CASE("GpuTelemetry: no helper path leaves stats unavailable", "[observability][gpu]") {
  GpuTelemetry t;
  auto s = t.latest();
  REQUIRE_FALSE(s.available);
  REQUIRE(s.reason == "no_helper_path");
  REQUIRE_FALSE(t.try_fetch_blocking(50ms).has_value());
}

TEST_CASE("GpuTelemetry: poll thread updates latest when helper succeeds", "[observability][gpu]") {
  const auto dir = test_helpers::make_temp_dir("gpu_ok");
  const auto helper = test_helpers::write_fake_helper(
    dir,
    "{\"available\":true,\"provider\":\"amd_adlx\",\"gpu\":{"
    "\"name\":\"R9700\",\"utilization\":42.5,\"vramMb\":17000.0,"
    "\"temperature\":55.0,\"power\":180.0,\"fanSpeed\":30.0,\"hotspotTemperature\":65.0}}"
  );
  GpuTelemetry t;
  t.set_helper_path(helper);
  t.set_poll_interval(50ms);
  t.set_max_staleness(500ms);
  t.start();
  std::this_thread::sleep_for(250ms);
  auto s = t.latest();
  t.stop();
  REQUIRE(s.available);
  REQUIRE(s.provider == "amd_adlx");
  REQUIRE(s.utilization_pct == 42.5);
  REQUIRE(s.vram_mb == 17000.0);
  REQUIRE(s.temperature_c == 55.0);
  REQUIRE(s.power_w == 180.0);
  REQUIRE(s.timestamp_unix_ms > 0);
  auto opt = t.try_fetch_blocking(100ms);
  REQUIRE(opt.has_value());
  REQUIRE(opt->utilization_pct == 42.5);
}

TEST_CASE("GpuTelemetry: graceful failure when helper missing", "[observability][gpu]") {
  GpuTelemetry t;
  t.set_helper_path("C:/nonexistent/inferdeck-adlx-helper.exe");
  t.set_poll_interval(50ms);
  t.start();
  std::this_thread::sleep_for(150ms);
  auto s = t.latest();
  t.stop();
  REQUIRE_FALSE(s.available);
  REQUIRE(s.reason == "helper_unreachable");
  REQUIRE_FALSE(t.try_fetch_blocking(100ms).has_value());
}

TEST_CASE("GpuTelemetry: stop is idempotent and joinable", "[observability][gpu]") {
  GpuTelemetry t;
  t.set_helper_path("C:/nope.exe");
  t.set_poll_interval(50ms);
  t.start();
  std::this_thread::sleep_for(60ms);
  t.stop();
  REQUIRE_FALSE(t.running());
  t.stop();
  REQUIRE_FALSE(t.running());
}

TEST_CASE("GpuTelemetry: record_external_sample sets latest", "[observability][gpu]") {
  GpuTelemetry t;
  GpuStats s;
  s.available = true;
  s.provider = "test";
  s.utilization_pct = 99.0;
  s.timestamp_unix_ms = static_cast<std::int64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  t.record_external_sample(s);
  auto got = t.latest();
  REQUIRE(got.available);
  REQUIRE(got.utilization_pct == 99.0);
  REQUIRE(got.timestamp_unix_ms == s.timestamp_unix_ms);
  auto opt = t.try_fetch_blocking(100ms);
  REQUIRE(opt.has_value());
  REQUIRE(opt->utilization_pct == 99.0);
}

TEST_CASE("GpuTelemetry: stale sample rejected", "[observability][gpu]") {
  GpuTelemetry t;
  t.set_max_staleness(10ms);
  GpuStats s;
  s.available = true;
  s.timestamp_unix_ms = 1;
  t.record_external_sample(s);
  std::this_thread::sleep_for(50ms);
  REQUIRE_FALSE(t.try_fetch_blocking(50ms).has_value());
}

// sentinel edit to test writability

