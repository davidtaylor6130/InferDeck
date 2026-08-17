#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace inferdeck::observability {

namespace detail {

struct GpuEngineCounterSample {
  std::string instance;
  double utilization_pct{};
};

class GpuUtilizationWindow {
public:
  explicit GpuUtilizationWindow(std::int64_t window_ms = 4000);
  double add(std::int64_t timestamp_ms,
             const std::vector<GpuEngineCounterSample>& samples);

private:
  struct Entry {
    double duration_ms{};
    std::unordered_map<std::string, double> engines;
  };

  std::int64_t window_ms_;
  std::int64_t last_timestamp_ms_{};
  double duration_ms_{};
  bool initialized_{false};
  std::deque<Entry> entries_;
};

}

struct GpuStats {
  bool available{false};
  std::string provider;
  std::string reason;
  std::int64_t timestamp_unix_ms{};
  std::string gpu_name;
  double utilization_pct{};
  double vram_mb{};
  double vram_total_mb{};
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

  mutable std::mutex lifecycle_mtx_;
  mutable std::mutex mtx_;
  std::condition_variable sample_cv_;
  std::condition_variable poll_cv_;
  GpuStats latest_;

  std::atomic<bool> running_{false};
  std::thread worker_;
};

}
