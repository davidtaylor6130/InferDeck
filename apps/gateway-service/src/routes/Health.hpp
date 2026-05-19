#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleHealth(const httplib::Request& req, httplib::Response& resp);
}
