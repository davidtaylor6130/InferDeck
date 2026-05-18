/// @file FineTuningJobs.hpp
/// @brief /v1/fine_tuning/jobs route handlers.

#pragma once

#include <httplib.h>

namespace inferdeck::gateway::routes {

/// Handle GET /v1/fine_tuning/jobs
void HandleFineTuningJobsList(const httplib::Request& req, httplib::Response& resp);

/// Handle POST /v1/fine_tuning/jobs
void HandleFineTuningJobsCreate(const httplib::Request& req, httplib::Response& resp);

/// Handle GET /v1/fine_tuning/jobs/{job_id}
void HandleFineTuningJobsGet(const httplib::Request& req, httplib::Response& resp);

/// Handle POST /v1/fine_tuning/jobs/{job_id}/cancel
void HandleFineTuningJobsCancel(const httplib::Request& req, httplib::Response& resp);

/// Validate fine-tuning create request.
std::string ValidateFineTuningCreate(const nlohmann::json& body);

} // namespace inferdeck::gateway::routes
