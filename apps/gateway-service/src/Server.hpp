/// @file Server.hpp
/// @brief HTTP/S server for InferDeck gateway.
///
/// Wraps cpp-httplib to provide HTTPS server with SSE streaming support.
/// Handles TLS configuration, route registration, and graceful shutdown.

#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <httplib.h>

namespace inferdeck::gateway {

/// Server configuration.
struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
    bool tls_enabled = true;
    std::string cert_file = "certs/server.crt";
    std::string key_file = "certs/server.key";
    int max_connections = 100;
    int request_timeout_ms = 30000;
};

/// HTTP response code alias.
using HttpCode = int;

/// HTTP request handler type.
using RequestHandler = std::function<void(const httplib::Request&, httplib::Response&)>;

/// GatewayServer manages the HTTP/S server lifecycle.
///
/// Provides methods to start, stop, and configure the HTTPS server.
/// Handles TLS setup and route registration for OpenAI-compatible endpoints.
class GatewayServer {
public:
    /// Create a new GatewayServer instance.
    GatewayServer();
    ~GatewayServer();

    /// Initialize the server with configuration.
    /// @param config Server configuration.
    /// @return True if initialization succeeded.
    bool Initialize(const ServerConfig& config);

    /// Start the server (non-blocking).
    /// @return True if server started successfully.
    bool Start();

    /// Stop the server (blocking until shutdown complete).
    void Stop();

    /// Wait for the server to be ready.
    void WaitForReady();

    /// Register a route handler.
    /// @param method HTTP method (GET, POST, etc.).
    /// @param path URL path pattern.
    /// @param handler Request handler function.
    void RegisterRoute(const std::string& method,
                       const std::string& path,
                       RequestHandler handler);

    /// Set the request timeout.
    /// @param timeout_ms Timeout in milliseconds.
    void SetTimeout(int timeout_ms);

    /// Get the server's base URL.
    /// @return The base URL string.
    std::string GetBaseUrl() const;

    /// Check if the server is running.
    /// @return True if the server is active.
    bool IsRunning() const;

    /// Get the current server config.
    /// @return Reference to the current config.
    const ServerConfig& GetConfig() const { return config_; }

private:
    ServerConfig config_;
    std::unique_ptr<httplib::SSLServer> ssl_server_;
    std::unique_ptr<httplib::Server> http_server_;
    std::thread server_thread_;
    std::atomic<bool> running_;
    std::mutex server_mutex_;
};

} // namespace inferdeck::gateway
