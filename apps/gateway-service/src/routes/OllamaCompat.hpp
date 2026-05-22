#pragma once
#include <httplib.h>

namespace inferdeck::gateway::routes {
void HandleOllamaVersion(const httplib::Request& req, httplib::Response& resp);
void HandleOllamaTags(const httplib::Request& req, httplib::Response& resp);
void HandleOllamaChat(const httplib::Request& req, httplib::Response& resp);
}
