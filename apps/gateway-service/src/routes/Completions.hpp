/// @file Completions.hpp
/// @brief /v1/completions route handler.

#pragma once

#include <httplib.h>

namespace inferdeck::gateway::routes {

/// Handle /v1/completions POST request.
void HandleCompletions(const httplib::Request& req, httplib::Response& resp);

/// Handle streaming /v1/completions request.
void HandleCompletionsStream(const httplib::Request& req, httplib::Response& resp);

} // namespace inferdeck::gateway::routes
