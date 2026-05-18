/// @file Images.hpp
/// @brief /v1/images/generate route handler.

#pragma once

#include <httplib.h>

namespace inferdeck::gateway::routes {

/// Handle POST /v1/images/generate
void HandleImageGenerate(const httplib::Request& req, httplib::Response& resp);

/// Validate image generation request body.
std::string ValidateImageRequest(const nlohmann::json& body);

} // namespace inferdeck::gateway::routes
