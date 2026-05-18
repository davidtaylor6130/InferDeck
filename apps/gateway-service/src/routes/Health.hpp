/// @file Health.hpp
/// @brief /v1/health route handler.

#pragma once

#include <httplib.h>

namespace inferdeck::gateway::routes {

/// Handle /v1/health GET request.
void HandleHealth(const httplib::Request& req, httplib::Response& resp);

} // namespace inferdeck::gateway::routes
