#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <httplib.h>

namespace inferdeck::gateway {

struct ServerConfig {
    std::string host = "0.0.0.0";
    int dashboardPort = 8080;
    int apiPort = 11434;
    bool tls_enabled = false;
    std::string cert_file;
    std::string key_file;
    int max_connections = 100;
    int request_timeout_ms = 300000;
    std::string model_path;
    std::string model_directory;
    std::string precision = "auto";
    int n_gpu_layers = -1;
    int context_size = 100000;
};

using HttpCode = int;
using RequestHandler = std::function<void(const httplib::Request&, httplib::Response&)>;

class GatewayServer {
public:
    GatewayServer();
    ~GatewayServer();

    bool Initialize(const ServerConfig& config);
    bool Start();
    void Stop();
    void WaitForReady();

    void RegisterApiRoute(const std::string& method, const std::string& path, RequestHandler handler);
    void RegisterDashboardRoute(const std::string& method, const std::string& path, RequestHandler handler);
    void SetDashboardMountPoint(const std::string& base, const std::string& dir);
    void SetTimeout(int timeout_ms);

    std::string GetDashboardUrl() const;
    std::string GetApiUrl() const;
    bool IsRunning() const;
    const ServerConfig& GetConfig() const { return config_; }

    httplib::Server& ApiServer();
    httplib::Server& DashboardServer();

private:
    ServerConfig config_;
    std::unique_ptr<httplib::Server> dashboard_server_;
    std::unique_ptr<httplib::Server> api_server_;
    std::thread dashboard_thread_;
    std::thread api_thread_;
    std::atomic<bool> running_;
    std::mutex server_mutex_;
};

} // namespace inferdeck::gateway
