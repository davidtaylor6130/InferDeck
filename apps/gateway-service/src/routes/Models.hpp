/// @file Models.hpp
/// @brief /v1/models route handler.

#pragma once

#include <httplib.h>

namespace inferdeck::gateway::routes {

/// Handle /v1/models GET request.
void HandleModels(const httplib::Request& req, httplib::Response& resp);

} // namespace inferdeck::gateway::routes
