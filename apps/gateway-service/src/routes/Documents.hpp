#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleDocumentsList(const httplib::Request& req, httplib::Response& resp);
void HandleDocumentsCreate(const httplib::Request& req, httplib::Response& resp);
void HandleDocumentsSearch(const httplib::Request& req, httplib::Response& resp);
}
