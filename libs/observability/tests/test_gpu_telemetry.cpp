#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include "observability/gpu_telemetry.hpp"
#include "test_helpers.hpp"

using namespace inferdeck::observability;
using namespace std::chrono_literals;

namespace {

GpuStats wait_for_sample(GpuTelemetry& telemetry, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  GpuStats sample;
  do {
    sample = telemetry.latest();
    if (sample.timestamp_unix_ms > 0 && !sample.provider.empty()) return sample;
    std::this_thread::sleep_for(50ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return sample;
}

}

TEST_CASE("GpuTelemetry: no helper path leaves stats unavailable", "[observability][gpu]") {
  GpuTelemetry t;
  auto s = t.latest();
  REQUIRE_FALSE(s.available);
  REQUIRE(s.reason == "no_helper_path");
  REQUIRE_FALSE(t.try_fetch_blocking(50ms).has_value());
}

TEST_CASE("GpuTelemetry: poll thread publishes an in-process provider sample", "[observability][gpu]") {
  GpuTelemetry t;
  t.set_poll_interval(50ms);
  t.set_max_staleness(500ms);
  t.start();
  auto s = wait_for_sample(t, 2s);
  t.stop();
  REQUIRE_FALSE(s.provider.empty());
  REQUIRE(s.timestamp_unix_ms > 0);
  if (s.available) {
    auto opt = t.try_fetch_blocking(100ms);
    REQUIRE(opt.has_value());
    REQUIRE(opt->provider == s.provider);
  }
}

TEST_CASE("GpuTelemetry: helper path is ignored by in-process telemetry", "[observability][gpu]") {
  GpuTelemetry t;
  t.set_helper_path("C:/nonexistent/inferdeck-adlx-helper.exe");
  t.set_poll_interval(50ms);
  t.start();
  auto s = wait_for_sample(t, 2s);
  t.stop();
  REQUIRE_FALSE(s.provider.empty());
  REQUIRE(s.timestamp_unix_ms > 0);
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
