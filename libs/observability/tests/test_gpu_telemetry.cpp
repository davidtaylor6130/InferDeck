#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <thread>

#include "observability/gpu_telemetry.hpp"
#include "test_helpers.hpp"

using namespace inferdeck::observability;
using namespace std::chrono_literals;
using Catch::Approx;

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

TEST_CASE("GpuTelemetry: stop interrupts a long poll wait", "[observability][gpu]") {
  GpuTelemetry t;
  t.set_poll_interval(5s);
  t.start();
  const auto sample = wait_for_sample(t, 2s);
  REQUIRE(sample.timestamp_unix_ms > 0);

  const auto started = std::chrono::steady_clock::now();
  t.stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE_FALSE(t.running());
  REQUIRE(elapsed < 500ms);
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

TEST_CASE("GpuTelemetry: blocking fetch wakes for a fresh external sample",
          "[observability][gpu]") {
  GpuTelemetry t;
  GpuStats s;
  s.available = true;
  s.provider = "test";
  s.timestamp_unix_ms = static_cast<std::int64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());

  std::thread publisher([&] {
    std::this_thread::sleep_for(25ms);
    t.record_external_sample(s);
  });
  const auto started = std::chrono::steady_clock::now();
  const auto sample = t.try_fetch_blocking(500ms);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  publisher.join();

  REQUIRE(sample.has_value());
  REQUIRE(sample->provider == "test");
  REQUIRE(elapsed >= 10ms);
  REQUIRE(elapsed < 500ms);
}

TEST_CASE("GpuTelemetry: delayed engine accounting preserves utilization area",
          "[observability][gpu]") {
  detail::GpuUtilizationWindow window;
  const std::string engine =
      "pid_1_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_compute";
  REQUIRE(window.add(1000, {{engine, 0.0}}) == 0.0);
  REQUIRE(window.add(2000, {{engine, 0.0}}) == 0.0);
  REQUIRE(window.add(3000, {{engine, 0.0}}) == 0.0);
  REQUIRE(window.add(4000, {{engine, 0.0}}) == 0.0);
  REQUIRE(window.add(5000, {{engine, 400.0}}) == Approx(100.0));
}

TEST_CASE("GpuTelemetry: process contributions are grouped by physical engine",
          "[observability][gpu]") {
  detail::GpuUtilizationWindow window;
  window.add(1000, {});
  const double utilization = window.add(2000, {
      {"pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_compute", 40.0},
      {"pid_2_luid_0x0_0x1_phys_0_eng_0_engtype_compute", 35.0},
      {"pid_2_luid_0x0_0x1_phys_0_eng_1_engtype_copy", 60.0},
  });
  REQUIRE(utilization == Approx(75.0));
}

TEST_CASE("GpuTelemetry: final busiest-engine utilization is clamped",
          "[observability][gpu]") {
  detail::GpuUtilizationWindow window;
  window.add(1000, {});
  REQUIRE(window.add(2000, {
      {"pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_compute", 90.0},
      {"pid_2_luid_0x0_0x1_phys_0_eng_0_engtype_compute", 80.0},
      {"pid_2_luid_0x0_0x1_phys_0_eng_1_engtype_copy", 40.0},
  }) == Approx(100.0));
}
