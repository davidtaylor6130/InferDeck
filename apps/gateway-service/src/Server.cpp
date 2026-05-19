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
    http_server_ = std::make_unique<httplib::Server>();
    Logger::Get().Info("HTTP server initialized");
    SetTimeout(config_.request_timeout_ms);
    return true;
}

bool GatewayServer::Start() {
    if (!http_server_) {
        Logger::Get().Error("Server not initialized");
        return false;
    }

    running_ = true;
    server_thread_ = std::thread([this]() {
        http_server_->listen(config_.host.c_str(), config_.port);
    });

    Logger::Get().Info("Server listening on " + config_.host + ":" + std::to_string(config_.port));
    return true;
}

void GatewayServer::Stop() {
    running_ = false;
    if (http_server_) {
        http_server_->stop();
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    Logger::Get().Info("Server stopped");
}

void GatewayServer::WaitForReady() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void GatewayServer::RegisterRoute(const std::string& method, const std::string& path, RequestHandler handler) {
    auto wrapper = [handler](const httplib::Request& req, httplib::Response& resp) {
        handler(req, resp);
    };

    if (method == "GET") http_server_->Get(path, wrapper);
    else if (method == "POST") http_server_->Post(path, wrapper);
    else if (method == "PUT") http_server_->Put(path, wrapper);
    else if (method == "DELETE") http_server_->Delete(path, wrapper);
}

void GatewayServer::SetTimeout(int timeout_ms) {
    if (http_server_) {
        http_server_->set_read_timeout(timeout_ms / 1000);
        http_server_->set_write_timeout(timeout_ms / 1000);
    }
}

std::string GatewayServer::GetBaseUrl() const {
    return "http://" + config_.host + ":" + std::to_string(config_.port);
}

bool GatewayServer::IsRunning() const {
    return running_;
}

} // namespace inferdeck::gateway
