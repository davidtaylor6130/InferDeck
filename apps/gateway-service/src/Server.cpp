#include "Server.hpp"
#include "core/Logger.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using inferdeck::core::Logger;

namespace inferdeck::gateway {

GatewayServer::GatewayServer() : running_(false) {
}

GatewayServer::~GatewayServer() {
    if (running_) {
        Stop();
    }
}

bool GatewayServer::Initialize(const ServerConfig& config) {
    config_ = config;
    dashboard_server_ = std::make_unique<httplib::Server>();
    api_server_ = std::make_unique<httplib::Server>();

    api_server_->set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With"},
        {"Access-Control-Max-Age", "86400"}
    });

    api_server_->Options("/.*", [](const httplib::Request&, httplib::Response& resp) {
        resp.status = 204;
    });

    Logger::Get().Info("Dashboard server initialized");
    Logger::Get().Info("API server initialized");
    SetTimeout(config_.request_timeout_ms);
    return true;
}

bool GatewayServer::Start() {
    if (!dashboard_server_ || !api_server_) {
        Logger::Get().Error("Servers not initialized");
        return false;
    }

    running_ = true;

    dashboard_thread_ = std::thread([this]() {
        Logger::Get().Info("Dashboard listening on " + config_.host + ":" + std::to_string(config_.dashboardPort));
        dashboard_server_->listen(config_.host.c_str(), config_.dashboardPort);
    });

    api_thread_ = std::thread([this]() {
        Logger::Get().Info("API listening on " + config_.host + ":" + std::to_string(config_.apiPort));
        api_server_->listen(config_.host.c_str(), config_.apiPort);
    });

    Logger::Get().Info("Dashboard: " + GetDashboardUrl());
    Logger::Get().Info("API: " + GetApiUrl());
    return true;
}

void GatewayServer::Stop() {
    running_ = false;
    if (dashboard_server_) dashboard_server_->stop();
    if (api_server_) api_server_->stop();
    if (dashboard_thread_.joinable()) dashboard_thread_.join();
    if (api_thread_.joinable()) api_thread_.join();
    Logger::Get().Info("Servers stopped");
}

void GatewayServer::WaitForReady() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void GatewayServer::RegisterApiRoute(const std::string& method, const std::string& path, RequestHandler handler) {
    auto wrapper = [handler](const httplib::Request& req, httplib::Response& resp) {
        handler(req, resp);
    };
    if (method == "GET") api_server_->Get(path, wrapper);
    else if (method == "POST") api_server_->Post(path, wrapper);
    else if (method == "PUT") api_server_->Put(path, wrapper);
    else if (method == "DELETE") api_server_->Delete(path, wrapper);
}

void GatewayServer::RegisterDashboardRoute(const std::string& method, const std::string& path, RequestHandler handler) {
    auto wrapper = [handler](const httplib::Request& req, httplib::Response& resp) {
        handler(req, resp);
    };
    if (method == "GET") dashboard_server_->Get(path, wrapper);
    else if (method == "POST") dashboard_server_->Post(path, wrapper);
}

void GatewayServer::SetDashboardMountPoint(const std::string& base, const std::string& dir) {
    if (dashboard_server_) {
        dashboard_server_->set_mount_point(base, dir);
        Logger::Get().Info("Dashboard static files mounted: " + base + " -> " + dir);
    }
}

void GatewayServer::SetTimeout(int timeout_ms) {
    if (dashboard_server_) {
        dashboard_server_->set_read_timeout(timeout_ms / 1000);
        dashboard_server_->set_write_timeout(timeout_ms / 1000);
    }
    if (api_server_) {
        api_server_->set_read_timeout(timeout_ms / 1000);
        api_server_->set_write_timeout(timeout_ms / 1000);
    }
}

std::string GatewayServer::GetDashboardUrl() const {
    return "http://" + config_.host + ":" + std::to_string(config_.dashboardPort);
}

std::string GatewayServer::GetApiUrl() const {
    return "http://" + config_.host + ":" + std::to_string(config_.apiPort);
}

bool GatewayServer::IsRunning() const {
    return running_;
}

httplib::Server& GatewayServer::ApiServer() {
    return *api_server_;
}

httplib::Server& GatewayServer::DashboardServer() {
    return *dashboard_server_;
}

} // namespace inferdeck::gateway
