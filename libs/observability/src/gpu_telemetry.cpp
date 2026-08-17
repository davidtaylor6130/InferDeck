#include "observability/gpu_telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cwchar>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

namespace inferdeck::observability {

namespace {

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::int64_t steady_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string gpu_engine_key(const std::string& instance) {
  const auto luid = instance.find("luid_");
  return luid == std::string::npos ? instance : instance.substr(luid);
}

#ifdef _WIN32
std::string narrow(const wchar_t* value) {
  if (!value) return {};
  const auto length = static_cast<int>(std::wcslen(value));
  if (length == 0) return {};
  const int size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value, length, nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(
          CP_UTF8, WC_ERR_INVALID_CHARS, value, length, out.data(), size, nullptr, nullptr) != size) {
    return {};
  }
  return out;
}

class PdhGpuEngineCounter {
public:
  explicit PdhGpuEngineCounter(const wchar_t* path) : path_(path) {}
  ~PdhGpuEngineCounter() {
    if (query_) PdhCloseQuery(query_);
  }

  std::optional<std::vector<detail::GpuEngineCounterSample>> read() {
    if (!ensure()) return std::nullopt;
    if (PdhCollectQueryData(query_) != ERROR_SUCCESS) return std::nullopt;
    DWORD buffer_size = 0;
    DWORD item_count = 0;
    auto status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &buffer_size, &item_count, nullptr);
    if (status != PDH_MORE_DATA || buffer_size == 0 || item_count == 0) return std::nullopt;
    std::vector<std::byte> buffer(buffer_size);
    auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &buffer_size, &item_count, items);
    if (status != ERROR_SUCCESS) return std::nullopt;
    std::vector<detail::GpuEngineCounterSample> samples;
    samples.reserve(item_count);
    for (DWORD i = 0; i < item_count; ++i) {
      if (items[i].FmtValue.CStatus == ERROR_SUCCESS) {
        samples.push_back({narrow(items[i].szName), items[i].FmtValue.doubleValue});
      }
    }
    return samples;
  }

private:
  bool ensure() {
    if (initialized_) return query_ && counter_;
    initialized_ = true;
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return false;
    if (PdhAddEnglishCounterW(query_, path_, 0, &counter_) != ERROR_SUCCESS) {
      PdhCloseQuery(query_);
      query_ = nullptr;
      return false;
    }
    PdhCollectQueryData(query_);
    return true;
  }

  const wchar_t* path_;
  PDH_HQUERY query_{nullptr};
  PDH_HCOUNTER counter_{nullptr};
  bool initialized_{false};
};

class PdhLargeSumCounter {
public:
  explicit PdhLargeSumCounter(const wchar_t* path) : path_(path) {}
  ~PdhLargeSumCounter() {
    if (query_) PdhCloseQuery(query_);
  }

  std::optional<std::uint64_t> read() {
    if (!ensure()) return std::nullopt;
    if (PdhCollectQueryData(query_) != ERROR_SUCCESS) return std::nullopt;
    DWORD buffer_size = 0;
    DWORD item_count = 0;
    auto status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_LARGE, &buffer_size, &item_count, nullptr);
    if (status != PDH_MORE_DATA || buffer_size == 0 || item_count == 0) return std::nullopt;
    std::vector<std::byte> buffer(buffer_size);
    auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_LARGE, &buffer_size, &item_count, items);
    if (status != ERROR_SUCCESS) return std::nullopt;
    std::uint64_t total = 0;
    for (DWORD i = 0; i < item_count; ++i) {
      if (items[i].FmtValue.CStatus == ERROR_SUCCESS && items[i].FmtValue.largeValue > 0) {
        total += static_cast<std::uint64_t>(items[i].FmtValue.largeValue);
      }
    }
    return total;
  }

private:
  bool ensure() {
    if (initialized_) return query_ && counter_;
    initialized_ = true;
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return false;
    if (PdhAddEnglishCounterW(query_, path_, 0, &counter_) != ERROR_SUCCESS) {
      PdhCloseQuery(query_);
      query_ = nullptr;
      return false;
    }
    PdhCollectQueryData(query_);
    return true;
  }

  const wchar_t* path_;
  PDH_HQUERY query_{nullptr};
  PDH_HCOUNTER counter_{nullptr};
  bool initialized_{false};
};

std::optional<std::pair<std::string, std::uint64_t>> read_dxgi_adapter() {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
    return std::nullopt;
  }
  std::optional<std::pair<std::string, std::uint64_t>> best;
  for (UINT index = 0;; ++index) {
    IDXGIAdapter1* adapter = nullptr;
    const auto status = factory->EnumAdapters1(index, &adapter);
    if (status == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(status) || !adapter) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
        desc.DedicatedVideoMemory > 0) {
      best = {narrow(desc.Description), static_cast<std::uint64_t>(desc.DedicatedVideoMemory)};
      adapter->Release();
      break;
    }
    adapter->Release();
  }
  factory->Release();
  return best;
}
#endif

} // namespace

detail::GpuUtilizationWindow::GpuUtilizationWindow(std::int64_t window_ms)
    : window_ms_(std::max<std::int64_t>(1, window_ms)) {}

double detail::GpuUtilizationWindow::add(
    std::int64_t timestamp_ms,
    const std::vector<GpuEngineCounterSample>& samples) {
  std::unordered_map<std::string, double> engines;
  for (const auto& sample : samples) {
    if (!std::isfinite(sample.utilization_pct) || sample.utilization_pct <= 0.0) continue;
    engines[gpu_engine_key(sample.instance)] += sample.utilization_pct;
  }
  if (!initialized_) {
    initialized_ = true;
    last_timestamp_ms_ = timestamp_ms;
    return 0.0;
  }
  if (timestamp_ms <= last_timestamp_ms_) return 0.0;

  const double elapsed_ms = static_cast<double>(timestamp_ms - last_timestamp_ms_);
  last_timestamp_ms_ = timestamp_ms;
  entries_.push_back({elapsed_ms, std::move(engines)});
  duration_ms_ += elapsed_ms;

  while (!entries_.empty() && duration_ms_ > static_cast<double>(window_ms_)) {
    const double excess = duration_ms_ - static_cast<double>(window_ms_);
    if (entries_.front().duration_ms <= excess) {
      duration_ms_ -= entries_.front().duration_ms;
      entries_.pop_front();
    } else {
      entries_.front().duration_ms -= excess;
      duration_ms_ -= excess;
    }
  }

  if (duration_ms_ <= 0.0) return 0.0;
  std::unordered_map<std::string, double> weighted;
  for (const auto& entry : entries_) {
    for (const auto& [engine, utilization] : entry.engines) {
      weighted[engine] += utilization * entry.duration_ms;
    }
  }
  double busiest = 0.0;
  for (const auto& [_, total] : weighted) {
    busiest = std::max(busiest, total / duration_ms_);
  }
  return std::clamp(busiest, 0.0, 100.0);
}

GpuTelemetry::GpuTelemetry() {
  latest_.available = false;
  latest_.reason = "no_helper_path";
  latest_.timestamp_unix_ms = 0;
}
GpuTelemetry::~GpuTelemetry() { stop(); }

void GpuTelemetry::set_helper_path(std::string path) {
  std::lock_guard lk(mtx_);
  helper_path_ = std::move(path);
}

void GpuTelemetry::set_poll_interval(std::chrono::milliseconds interval) {
  {
    std::lock_guard lk(mtx_);
    poll_interval_ = std::max(interval, std::chrono::milliseconds{1});
  }
  poll_cv_.notify_all();
}

void GpuTelemetry::set_max_staleness(std::chrono::milliseconds max) {
  {
    std::lock_guard lk(mtx_);
    max_staleness_ = std::max(max, std::chrono::milliseconds{0});
  }
  sample_cv_.notify_all();
}

void GpuTelemetry::start() {
  std::lock_guard lifecycle_lk(lifecycle_mtx_);
  if (running_.exchange(true)) return;
  try {
    worker_ = std::thread([this] { run_loop(); });
  } catch (...) {
    running_.store(false);
    throw;
  }
}

void GpuTelemetry::stop() {
  std::lock_guard lifecycle_lk(lifecycle_mtx_);
  running_.store(false);
  poll_cv_.notify_all();
  sample_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

GpuStats GpuTelemetry::latest() const {
  std::lock_guard lk(mtx_);
  return latest_;
}

std::optional<GpuStats> GpuTelemetry::try_fetch_blocking(std::chrono::milliseconds timeout) {
  const auto bounded_timeout = std::max(timeout, std::chrono::milliseconds{0});
  std::unique_lock lk(mtx_);
  const auto fresh_sample_available = [this] {
    if (!latest_.available || latest_.timestamp_unix_ms == 0) return false;
    const auto age = now_ms() - latest_.timestamp_unix_ms;
    return age <= max_staleness_.count();
  };
  if (!fresh_sample_available() &&
      !sample_cv_.wait_for(lk, bounded_timeout, fresh_sample_available)) {
    return std::nullopt;
  }
  return latest_;
}

void GpuTelemetry::record_external_sample(const GpuStats& s) {
  {
    std::lock_guard lk(mtx_);
    latest_ = s;
  }
  sample_cv_.notify_all();
}

void GpuTelemetry::run_loop() {
  using namespace std::chrono;
#ifdef _WIN32
  PdhGpuEngineCounter gpu_util(L"\\GPU Engine(*)\\Utilization Percentage");
  PdhLargeSumCounter dedicated_usage(L"\\GPU Adapter Memory(*)\\Dedicated Usage");
  detail::GpuUtilizationWindow gpu_utilization;
  std::optional<std::pair<std::string, std::uint64_t>> adapter;
  auto next_adapter_refresh = steady_clock::time_point::min();
#endif
  while (running_.load()) {
    GpuStats s;
#ifdef _WIN32
    const auto steady_now = steady_clock::now();
    if (steady_now >= next_adapter_refresh) {
      adapter = read_dxgi_adapter();
      next_adapter_refresh = steady_now + (adapter ? seconds{30} : seconds{5});
    }
    auto util = gpu_util.read();
    auto used_bytes = dedicated_usage.read();
    if (adapter || util || used_bytes) {
      s.available = true;
      s.provider = "windows_pdh_dxgi";
      s.gpu_name = adapter ? adapter->first : "Windows GPU";
      s.utilization_pct = util
          ? gpu_utilization.add(steady_ms(), *util)
          : 0.0;
      s.vram_mb = used_bytes ? static_cast<double>(*used_bytes) / (1024.0 * 1024.0) : 0.0;
      if (adapter && adapter->second > 0) {
        const double total_mb = static_cast<double>(adapter->second) / (1024.0 * 1024.0);
        s.vram_total_mb = total_mb;
        if (s.vram_mb <= 0.0) s.vram_mb = 0.0;
        s.reason = "vram_total_mb=" + std::to_string(static_cast<int>(total_mb));
      }
    } else {
      s.available = false;
      s.provider = "windows_pdh_dxgi";
      s.reason = "windows_gpu_counters_unavailable";
    }
#else
    s.available = false;
    s.provider = "none";
    s.reason = "gpu_telemetry_windows_only";
#endif
    s.timestamp_unix_ms = now_ms();
    {
      std::lock_guard lk(mtx_);
      latest_ = s;
    }
    sample_cv_.notify_all();
    std::unique_lock lk(mtx_);
    const auto interval = poll_interval_;
    poll_cv_.wait_for(lk, interval);
  }
}

}
