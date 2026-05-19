#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleCompletions(const httplib::Request& req, httplib::Response& resp);
void HandleCompletionsStream(const httplib::Request& req, httplib::Response& resp);
}
