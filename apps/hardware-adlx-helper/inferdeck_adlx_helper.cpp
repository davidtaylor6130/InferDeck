#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#ifdef INFERDECK_WITH_ADLX
#include "SDK/ADLXHelper/Windows/Cpp/ADLXHelper.h"
#include "SDK/Include/IPerformanceMonitoring3.h"
#endif

#ifdef INFERDECK_WITH_ADLX
static ADLX_RESULT initialize_adlx(ADLXHelper& adlx, bool incompatible) {
  return incompatible ? adlx.InitializeWithIncompatibleDriver() : adlx.Initialize();
}
#endif

static std::string now_iso() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

static std::string escape_json(const std::string& value) {
  std::ostringstream out;
  for (const char c : value) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << c; break;
    }
  }
  return out.str();
}

static void unavailable(const std::string& reason) {
  std::cout
    << "{"
    << "\"available\":false,"
    << "\"provider\":\"amd_adlx\","
    << "\"reason\":\"" << reason << "\","
    << "\"timestamp\":\"" << now_iso() << "\""
    << "}\n";
}

int main(int argc, char** argv) {
  bool json = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--json") json = true;
  }
  if (!json) {
    std::cerr << "Usage: inferdeck-adlx-helper --json\n";
    return 2;
  }

#ifndef _WIN32
  unavailable("adlx_windows_only");
  return 0;
#endif

#ifndef INFERDECK_WITH_ADLX
  unavailable("built_without_adlx_sdk");
  return 0;
#else
  ADLX_RESULT res = ADLX_FAIL;
  ADLXHelper adlx;
  bool initialized = false;

  adlx::IADLXPerformanceMonitoringServicesPtr perf;
  adlx::IADLXGPUListPtr gpus;

  for (const bool incompatible : {false, true}) {
    if (initialized) {
      perf = nullptr;
      gpus = nullptr;
      adlx.Terminate();
    }
    res = initialize_adlx(adlx, incompatible);
    initialized = ADLX_SUCCEEDED(res);
    if (!initialized || adlx.GetSystemServices() == nullptr) continue;

    res = adlx.GetSystemServices()->GetPerformanceMonitoringServices(&perf);
    if (ADLX_FAILED(res) || !perf) continue;

    res = adlx.GetSystemServices()->GetGPUs(&gpus);
    if (ADLX_SUCCEEDED(res) && gpus && gpus->Size() > 0) break;
  }

  if (!initialized || adlx.GetSystemServices() == nullptr) {
    unavailable("adlx_initialize_failed");
    return 0;
  }
  if (!perf) {
    unavailable("adlx_performance_monitoring_unavailable");
    return 0;
  }
  if (!gpus || gpus->Size() == 0) {
    unavailable("adlx_no_gpu");
    return 0;
  }

  adlx::IADLXGPUPtr gpu;
  res = gpus->At(gpus->Begin(), &gpu);
  if (ADLX_FAILED(res) || !gpu) {
    unavailable("adlx_gpu_select_failed");
    return 0;
  }

  const char* raw_name = nullptr;
  gpu->Name(&raw_name);
  const std::string name = raw_name ? raw_name : "AMD GPU";

  adlx::IADLXGPUMetricsSupportPtr support;
  res = perf->GetSupportedGPUMetrics(gpu, &support);
  if (ADLX_FAILED(res) || !support) {
    unavailable("adlx_gpu_metrics_support_unavailable");
    return 0;
  }

  adlx::IADLXGPUMetricsPtr metrics;
  res = perf->GetCurrentGPUMetrics(gpu, &metrics);
  if (ADLX_FAILED(res) || !metrics) {
    unavailable("adlx_gpu_metrics_unavailable");
    return 0;
  }

  auto number_field = [](const char* name, double value, bool& first) {
    if (!first) std::cout << ",";
    first = false;
    std::cout << "\"" << name << "\":" << std::fixed << std::setprecision(2) << value;
  };

  std::cout << "{"
            << "\"available\":true,"
            << "\"provider\":\"amd_adlx\","
            << "\"timestamp\":\"" << now_iso() << "\","
            << "\"gpu\":{"
            << "\"name\":\"" << escape_json(name) << "\"";

  bool first_metric = false;
  adlx_bool supported = false;
  adlx_double double_value = 0;
  adlx_int int_value = 0;

  if (ADLX_SUCCEEDED(support->IsSupportedGPUTemperature(&supported)) && supported &&
      ADLX_SUCCEEDED(metrics->GPUTemperature(&double_value))) {
    number_field("temperature", double_value, first_metric);
  }
  if (ADLX_SUCCEEDED(support->IsSupportedGPUHotspotTemperature(&supported)) && supported &&
      ADLX_SUCCEEDED(metrics->GPUHotspotTemperature(&double_value))) {
    number_field("hotspotTemperature", double_value, first_metric);
  }
  if (ADLX_SUCCEEDED(support->IsSupportedGPUPower(&supported)) && supported &&
      ADLX_SUCCEEDED(metrics->GPUPower(&double_value))) {
    number_field("power", double_value, first_metric);
  }
  if (ADLX_SUCCEEDED(support->IsSupportedGPUFanSpeed(&supported)) && supported &&
      ADLX_SUCCEEDED(metrics->GPUFanSpeed(&int_value))) {
    number_field("fanSpeed", static_cast<double>(int_value), first_metric);
  }
  if (ADLX_SUCCEEDED(support->IsSupportedGPUUsage(&supported)) && supported &&
      ADLX_SUCCEEDED(metrics->GPUUsage(&double_value))) {
    number_field("utilization", double_value, first_metric);
  }
  if (ADLX_SUCCEEDED(support->IsSupportedGPUVRAM(&supported)) && supported &&
      ADLX_SUCCEEDED(metrics->GPUVRAM(&int_value))) {
    number_field("vramMb", static_cast<double>(int_value), first_metric);
  }

  std::cout << "},\"cpu\":{}}"
            << "\n";
  return 0;
#endif
}
