#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleFineTuningJobsList(const httplib::Request& req, httplib::Response& resp);
void HandleFineTuningJobsCreate(const httplib::Request& req, httplib::Response& resp);
}
