#pragma once

#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

#include "observability/gpu_telemetry.hpp"
#include "observability/metrics.hpp"
#include "observability/stats_db.hpp"

namespace inferdeck::gateway {

class MetricsBuilder {
public:
  static nlohmann::json build_live(const observability::Metrics& m,
                                   const observability::GpuTelemetry& gpu,
                                   std::int64_t uptime_seconds);

  static nlohmann::json build_history(const observability::StatsDb& db, int limit = 100);

  static nlohmann::json build_health(const observability::Metrics& m,
                                     const observability::GpuTelemetry& gpu,
                                     const observability::StatsDb& db);
};

}
