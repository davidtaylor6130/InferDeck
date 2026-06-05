#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "observability/metrics.hpp"

namespace inferdeck::observability {

struct RequestRow {
  std::int64_t timestamp_unix_ms{};
  std::string model;
  int prompt_tokens{};
  int completion_tokens{};
  double duration_ms{};
  double tokens_per_second{};
  int status_code{};
  int slot_id{-1};
};

struct SwapRow {
  std::int64_t timestamp_unix_ms{};
  std::string from_model;
  std::string to_model;
  double duration_ms{};
  bool success{};
  std::string error;
};

class StatsDb {
public:
  explicit StatsDb(const std::string& path);
  ~StatsDb();

  StatsDb(const StatsDb&) = delete;
  StatsDb& operator=(const StatsDb&) = delete;

  void record_request(const RequestRow& row);
  void record_swap(const SwapRow& row);

  std::vector<RequestRow> recent_requests(int limit = 100) const;
  std::vector<SwapRow>    recent_swaps(int limit = 100) const;

  bool healthy() const noexcept { return healthy_; }
  const std::string& path() const noexcept { return path_; }

private:
  void open();
  void close();

  std::string path_;
  void* db_{nullptr};
  mutable std::mutex mtx_;
  bool healthy_{false};
};

}
