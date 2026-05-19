#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleModels(const httplib::Request& req, httplib::Response& resp);
}
