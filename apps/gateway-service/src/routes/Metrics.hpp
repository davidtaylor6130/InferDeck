/// @file Metrics.hpp
/// @brief /inferdeck/metrics route handler.

#pragma once

#include <httplib.h>

namespace inferdeck::gateway::routes {

/// Handle /inferdeck/metrics GET request.
void HandleMetrics(const httplib::Request& req, httplib::Response& resp);

} // namespace inferdeck::gateway::routes
