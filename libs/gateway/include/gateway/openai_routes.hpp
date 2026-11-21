#pragma once

#include <httplib.h>

#include "gateway/routes.hpp"

namespace inferdeck::gateway {

void handle_embeddings(const httplib::Request& req, httplib::Response& resp,
                       const GatewayDeps& deps);

/// <summary>OpenAI Decisions API: typed questions answered with calibrated probabilities.</summary>
void handle_decisions(const httplib::Request& req, httplib::Response& resp,
                      const GatewayDeps& deps);

void handle_responses(const httplib::Request& req, httplib::Response& resp,
                      const GatewayDeps& deps);

} // namespace inferdeck::gateway
