#pragma once

#include <httplib.h>

#include "gateway/routes.hpp"
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
    std::function<std::int64_t()> uptime_seconds;
};

void register_dashboard_routes(httplib::Server& server, const DashboardDeps& deps,
                               const RouteWrapper& wrap);

} // namespace inferdeck::gateway
