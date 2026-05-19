#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleMetrics(const httplib::Request& req, httplib::Response& resp);
}
