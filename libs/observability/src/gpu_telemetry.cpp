#include "observability/gpu_telemetry.hpp"

#include <chrono>
#include <cstdio>
#include <future>
#include <sstream>
#include <stdexcept>

namespace inferdeck::observability {

namespace {

std::string run_capture(const std::string& cmd) {
  std::string result;
  std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
  if (!pipe) return {};
  char buf[512];
  while (fgets(buf, sizeof(buf), pipe.get())) result += buf;
  return result;
}

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      default: out += c;
    }
  }
  return out;
}

}

GpuTelemetry::GpuTelemetry() {
  latest_.available = false;
  latest_.reason = "no_helper_path";
  latest_.timestamp_unix_ms = 0;
}
GpuTelemetry::~GpuTelemetry() { stop(); }

void GpuTelemetry::set_helper_path(std::string path) { helper_path_ = std::move(path); }
void GpuTelemetry::set_poll_interval(std::chrono::milliseconds interval) { poll_interval_ = interval; }
void GpuTelemetry::set_max_staleness(std::chrono::milliseconds max) { max_staleness_ = max; }

void GpuTelemetry::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return;
  worker_ = std::thread([this] { run_loop(); });
}

void GpuTelemetry::stop() {
  if (!running_.exchange(false)) return;
  if (worker_.joinable()) worker_.join();
}

GpuStats GpuTelemetry::latest() const {
  std::lock_guard lk(mtx_);
  return latest_;
}

std::optional<GpuStats> GpuTelemetry::try_fetch_blocking(std::chrono::milliseconds timeout) {
  auto fut = std::async(std::launch::async, [this] {
    std::lock_guard lk(mtx_);
    return latest_;
  });
  if (fut.wait_for(timeout) != std::future_status::ready) return std::nullopt;
  auto s = fut.get();
  if (!s.available) return std::nullopt;
  if (s.timestamp_unix_ms == 0) return std::nullopt;
  const auto age = now_ms() - s.timestamp_unix_ms;
  if (age > max_staleness_.count()) return std::nullopt;
  return s;
}

void GpuTelemetry::record_external_sample(const GpuStats& s) {
  std::lock_guard lk(mtx_);
  latest_ = s;
}

void GpuTelemetry::run_loop() {
  using namespace std::chrono;
  while (running_.load()) {
    GpuStats s;
    s.timestamp_unix_ms = now_ms();
    if (helper_path_.empty()) {
      s.available = false;
      s.reason = "no_helper_path";
    } else {
      const std::string quoted = "\"" + helper_path_ + "\" --json";
      const std::string out = run_capture(quoted);
      if (out.empty()) {
        s.available = false;
        s.reason = "helper_unreachable";
      } else {
        s.available = true;
        s.provider = "amd_adlx";
        s.gpu_name = "AMD GPU";
        std::size_t pos = 0;
        auto find_number = [&](const std::string& key) -> double {
          std::string needle = "\"" + key + "\":";
          auto p = out.find(needle, pos);
          if (p == std::string::npos) return 0.0;
          p += needle.size();
          while (p < out.size() && (out[p] == ' ' || out[p] == '\t')) ++p;
          double v = 0.0;
          try { v = std::stod(out.substr(p)); } catch (...) { v = 0.0; }
          return v;
        };
        s.utilization_pct       = find_number("utilization");
        s.vram_mb               = find_number("vramMb");
        s.temperature_c         = find_number("temperature");
        s.power_w               = find_number("power");
        s.fan_speed_pct         = find_number("fanSpeed");
        s.hotspot_temperature_c = find_number("hotspotTemperature");
      }
    }
    {
      std::lock_guard lk(mtx_);
      latest_ = s;
    }
    std::this_thread::sleep_for(poll_interval_);
  }
}

}
