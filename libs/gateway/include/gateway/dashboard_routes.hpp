#pragma once

#include <httplib.h>

#include "gateway/routes.hpp"
#include "gateway/model_store.hpp"
#include "gateway/profile_benchmark.hpp"
#include "gateway/profile_benchmark_scheduler.hpp"
#include "observability/gpu_telemetry.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace inferdeck::gateway {

using RouteWrapper = std::function<httplib::Server::Handler(httplib::Server::Handler)>;

struct DashboardDeps {
    GatewayDeps gw;
    observability::GpuTelemetry& gpu;
    std::string log_file;
    std::string pricing_file{"data/pricing.json"};
    std::string base_config_file;
    std::string active_config_file;
    std::string running_config_revision;
    bool using_active_config{false};
    std::string config_fallback_reason;
    std::function<foundation::Result<void>(const std::string&)> validate_config;
    std::function<foundation::Result<void>()> request_config_reload;
    ModelStore* model_store{nullptr};
    std::function<std::int64_t()> uptime_seconds;
    ProfileBenchmarkManager* profile_benchmark{nullptr};
    ProfileBenchmarkScheduler* profile_benchmark_scheduler{nullptr};
};

void register_dashboard_routes(httplib::Server& server, const DashboardDeps& deps,
                               const RouteWrapper& wrap);

} // namespace inferdeck::gateway
