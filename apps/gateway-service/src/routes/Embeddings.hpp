/// @file Embeddings.hpp
/// @brief /v1/embeddings route handler.

#pragma once

#include <httplib.h>

namespace inferdeck::gateway::routes {

/// Handle /v1/embeddings POST request.
void HandleEmbeddings(const httplib::Request& req, httplib::Response& resp);

} // namespace inferdeck::gateway::routes
