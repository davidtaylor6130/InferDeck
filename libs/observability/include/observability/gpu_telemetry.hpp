#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace inferdeck::observability {

struct GpuStats {
  bool available{false};
  std::string provider;
  std::string reason;
  std::int64_t timestamp_unix_ms{};
  std::string gpu_name;
  double utilization_pct{};
  double vram_mb{};
  double temperature_c{};
  double power_w{};
  double fan_speed_pct{};
  double hotspot_temperature_c{};
};

class GpuTelemetry {
public:
  GpuTelemetry();
  ~GpuTelemetry();

  GpuTelemetry(const GpuTelemetry&) = delete;
  GpuTelemetry& operator=(const GpuTelemetry&) = delete;

  void set_helper_path(std::string path);
  void set_poll_interval(std::chrono::milliseconds interval);
  void set_max_staleness(std::chrono::milliseconds max_staleness);

  void start();
  void stop();
  bool running() const noexcept { return running_.load(); }

  GpuStats latest() const;
  std::optional<GpuStats> try_fetch_blocking(std::chrono::milliseconds timeout);

  void record_external_sample(const GpuStats& s);

private:
  void run_loop();

  std::string helper_path_;
  std::chrono::milliseconds poll_interval_{100};
  std::chrono::milliseconds max_staleness_{2000};

  mutable std::mutex mtx_;
  GpuStats latest_;

  std::atomic<bool> running_{false};
  std::thread worker_;
};

}
