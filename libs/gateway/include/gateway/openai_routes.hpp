#pragma once

#include <httplib.h>

#include "gateway/routes.hpp"

namespace inferdeck::gateway {

void handle_embeddings(const httplib::Request& req, httplib::Response& resp,
                       const GatewayDeps& deps);

void handle_responses(const httplib::Request& req, httplib::Response& resp,
                      const GatewayDeps& deps);

} // namespace inferdeck::gateway
