#pragma once

#include "Server.hpp"

#include <chrono>

namespace inferdeck::gateway::routes {

using GatewayStartTime = std::chrono::steady_clock::time_point;

void HandleDashboardHealth(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config, GatewayStartTime started_at);
void HandleDashboardStatus(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config, GatewayStartTime started_at);
void HandleDashboardModels(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config);
void HandleDashboardRunningModels(const httplib::Request& req, httplib::Response& resp);
void HandleDashboardLoadModel(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config);
void HandleDashboardUnloadModel(const httplib::Request& req, httplib::Response& resp);
void HandleDashboardRescanModels(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config);
void HandleDashboardServices(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config, GatewayStartTime started_at);
void HandleDashboardStartService(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config);
void HandleDashboardStopService(const httplib::Request& req, httplib::Response& resp);
void HandleDashboardRestartService(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config);
void HandleDashboardJobs(const httplib::Request& req, httplib::Response& resp);
void HandleDashboardJobDetail(const httplib::Request& req, httplib::Response& resp);
void HandleDashboardJobEvents(const httplib::Request& req, httplib::Response& resp);
void HandleDashboardJobResult(const httplib::Request& req, httplib::Response& resp);
void HandleDashboardCancelJob(const httplib::Request& req, httplib::Response& resp);
void HandleDashboardRetryJob(const httplib::Request& req, httplib::Response& resp);
void HandleDashboardQueueAction(const httplib::Request& req, httplib::Response& resp, const std::string& action);
void HandleDashboardLogs(const httplib::Request& req, httplib::Response& resp);
void HandleDashboardEvents(const httplib::Request& req, httplib::Response& resp);

} // namespace inferdeck::gateway::routes
