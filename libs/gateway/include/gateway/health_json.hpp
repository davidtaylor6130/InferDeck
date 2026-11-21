#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace inferdeck::gateway {

inline nlohmann::json health_body(const std::vector<std::string>& unhealthy,
                                  const std::vector<std::string>& degraded,
                                  bool gpu_lost_backoff, bool db_healthy) {
    const bool ok = unhealthy.empty() && !gpu_lost_backoff;
    nlohmann::json body = {{"ok", ok},
                           {"status", ok && degraded.empty() ? "ok" : "degraded"},
                           {"unhealthy_models", unhealthy},
                           {"degraded_models", degraded},
                           {"compute_degraded", !degraded.empty()},
                           {"db_healthy", db_healthy}};
    if (gpu_lost_backoff) body["reason"] = "gpu_lost_backoff";
    else if (!unhealthy.empty()) body["reason"] = "execution_unhealthy";
    else if (!degraded.empty()) body["reason"] = "gpu_unavailable";
    return body;
}

}
