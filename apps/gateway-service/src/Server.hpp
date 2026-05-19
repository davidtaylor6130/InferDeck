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
    int port = 8080;
    bool tls_enabled = false;
    std::string cert_file;
    std::string key_file;
    int max_connections = 100;
    int request_timeout_ms = 30000;
    std::string model_path;
    std::string precision = "auto";
    int n_gpu_layers = -1;
    int context_size = 4096;
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
    void RegisterRoute(const std::string& method, const std::string& path, RequestHandler handler);
    void SetTimeout(int timeout_ms);
    std::string GetBaseUrl() const;
    bool IsRunning() const;
    const ServerConfig& GetConfig() const { return config_; }

private:
    ServerConfig config_;
    std::unique_ptr<httplib::Server> http_server_;
    std::thread server_thread_;
    std::atomic<bool> running_;
    std::mutex server_mutex_;
};

} // namespace inferdeck::gateway
