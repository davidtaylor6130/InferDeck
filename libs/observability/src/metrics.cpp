#include "observability/metrics.hpp"

#include <algorithm>
#include <stdexcept>

namespace inferdeck::observability {

void Metrics::record_request(const RequestRecord& rec) {
  total_requests_.fetch_add(1);
  total_prompt_tokens_.fetch_add(rec.prompt_tokens);
  total_completion_tokens_.fetch_add(rec.completion_tokens);
  total_duration_ms_.fetch_add(rec.duration_ms);

  std::lock_guard lk(mtx_);
  auto& s = by_model_[rec.model];
  s.requests += 1;
  s.prompt_tokens += rec.prompt_tokens;
  s.completion_tokens += rec.completion_tokens;
  s.total_duration_ms += rec.duration_ms;
  s.last_tokens_per_second = rec.tokens_per_second;
  s.last_timestamp_unix_ms = rec.timestamp_unix_ms;
  s.peak_tokens_per_second = std::max(s.peak_tokens_per_second, rec.tokens_per_second);
}

void Metrics::record_swap(const SwapRecord& rec) {
  (void)rec;
  total_swaps_.fetch_add(1);
}

void Metrics::record_gpu_sample(std::int64_t ts, double util, double vram, double temp, double power) {
  last_gpu_ts_.store(ts);
  last_gpu_util_.store(util);
  last_gpu_vram_mb_.store(vram);
  last_gpu_temp_c_.store(temp);
  last_gpu_power_w_.store(power);
}

double Metrics::avg_tokens_per_second() const {
  const double secs = total_duration_ms_.load() / 1000.0;
  if (secs <= 0.0) return 0.0;
  return static_cast<double>(total_completion_tokens_.load()) / secs;
}

ModelStats Metrics::snapshot_for(std::string_view model) const {
  std::lock_guard lk(mtx_);
  auto it = by_model_.find(std::string(model));
  if (it == by_model_.end()) return {};
  return it->second;
}

void Metrics::reset() {
  total_requests_.store(0);
  total_swaps_.store(0);
  total_prompt_tokens_.store(0);
  total_completion_tokens_.store(0);
  total_duration_ms_.store(0.0);
  std::lock_guard lk(mtx_);
  by_model_.clear();
  last_gpu_util_.store(0);
  last_gpu_vram_mb_.store(0);
  last_gpu_temp_c_.store(0);
  last_gpu_power_w_.store(0);
  last_gpu_ts_.store(0);
}

}
