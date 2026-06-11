#include "observability/gpu_telemetry.hpp"

#include <chrono>
#include <cstddef>
#include <future>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <thread>
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

#ifdef _WIN32
std::string narrow(const wchar_t* value) {
  if (!value) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(static_cast<std::size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr);
  return out;
}

class PdhDoubleSumCounter {
public:
  explicit PdhDoubleSumCounter(const wchar_t* path) : path_(path) {}
  ~PdhDoubleSumCounter() {
    if (query_) PdhCloseQuery(query_);
  }

  std::optional<double> read() {
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
    double total = 0.0;
    for (DWORD i = 0; i < item_count; ++i) {
      if (items[i].FmtValue.CStatus == ERROR_SUCCESS) total += items[i].FmtValue.doubleValue;
    }
    return std::clamp(total, 0.0, 100.0);
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
  IDXGIAdapter1* adapter = nullptr;
  for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
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
#ifdef _WIN32
    static PdhDoubleSumCounter gpu_util(L"\\GPU Engine(*)\\Utilization Percentage");
    static PdhLargeSumCounter dedicated_usage(L"\\GPU Adapter Memory(*)\\Dedicated Usage");
    auto adapter = read_dxgi_adapter();
    auto util = gpu_util.read();
    auto used_bytes = dedicated_usage.read();
    if (adapter || util || used_bytes) {
      s.available = true;
      s.provider = "windows_pdh_dxgi";
      s.gpu_name = adapter ? adapter->first : "Windows GPU";
      s.utilization_pct = util.value_or(0.0);
      s.vram_mb = used_bytes ? static_cast<double>(*used_bytes) / (1024.0 * 1024.0) : 0.0;
      if (adapter && adapter->second > 0) {
        const double total_mb = static_cast<double>(adapter->second) / (1024.0 * 1024.0);
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
    {
      std::lock_guard lk(mtx_);
      latest_ = s;
    }
    std::this_thread::sleep_for(poll_interval_);
  }
}

}
