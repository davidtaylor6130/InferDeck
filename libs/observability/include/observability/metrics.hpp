#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace inferdeck::observability {

struct RequestRecord {
  std::int64_t timestamp_unix_ms{};
  std::string model;
  int prompt_tokens{};
  int completion_tokens{};
  double duration_ms{};
  double tokens_per_second{};
  int status_code{};
  int slot_id{-1};
};

struct SwapRecord {
  std::int64_t timestamp_unix_ms{};
  std::string from_model;
  std::string to_model;
  double duration_ms{};
  bool success{};
  std::string error;
};

struct ModelStats {
  std::int64_t requests{};
  std::int64_t prompt_tokens{};
  std::int64_t completion_tokens{};
  double total_duration_ms{};
  double peak_tokens_per_second{};
  double last_tokens_per_second{};
  std::int64_t last_timestamp_unix_ms{};
};

class Metrics {
public:
  void record_request(const RequestRecord& rec);
  void record_swap(const SwapRecord& rec);
  void record_gpu_sample(std::int64_t ts, double util_pct, double vram_mb, double temp_c, double power_w);

  std::int64_t total_requests() const noexcept { return total_requests_.load(); }
  std::int64_t total_swaps()    const noexcept { return total_swaps_.load();    }
  std::int64_t total_prompt_tokens()     const noexcept { return total_prompt_tokens_.load(); }
  std::int64_t total_completion_tokens() const noexcept { return total_completion_tokens_.load(); }
  double total_duration_ms() const noexcept { return total_duration_ms_.load(); }

  std::int64_t lifetime_tokens_in()  const noexcept { return total_prompt_tokens_.load(); }
  std::int64_t lifetime_tokens_out() const noexcept { return total_completion_tokens_.load(); }
  double avg_tokens_per_second() const;

  ModelStats snapshot_for(std::string_view model) const;
  double last_gpu_util_pct() const noexcept { return last_gpu_util_; }
  double last_gpu_vram_mb()  const noexcept { return last_gpu_vram_mb_; }
  double last_gpu_temp_c()   const noexcept { return last_gpu_temp_c_; }
  double last_gpu_power_w()  const noexcept { return last_gpu_power_w_; }
  std::int64_t last_gpu_sample_unix_ms() const noexcept { return last_gpu_ts_; }

  void reset();

private:
  mutable std::mutex mtx_;
  std::unordered_map<std::string, ModelStats> by_model_;

  std::atomic<std::int64_t> total_requests_{};
  std::atomic<std::int64_t> total_swaps_{};
  std::atomic<std::int64_t> total_prompt_tokens_{};
  std::atomic<std::int64_t> total_completion_tokens_{};
  std::atomic<double>       total_duration_ms_{};

  std::atomic<double> last_gpu_util_{};
  std::atomic<double> last_gpu_vram_mb_{};
  std::atomic<double> last_gpu_temp_c_{};
  std::atomic<double> last_gpu_power_w_{};
  std::atomic<std::int64_t> last_gpu_ts_{};
};

}
