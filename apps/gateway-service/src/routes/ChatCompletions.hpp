#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleChatCompletions(const httplib::Request& req, httplib::Response& resp);
void HandleChatCompletionsStream(const httplib::Request& req, httplib::Response& resp);
}
