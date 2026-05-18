/// @file ConfigLoader.cpp
/// @brief Gateway configuration loader implementation.

#include "config/ConfigLoader.hpp"
#include "core/Logger.hpp"
#include "core/Config.hpp"

#include <yaml-cpp/yaml.h>
#include <fstream>

namespace inferdeck::gateway {

ServerConfig LoadConfig(const std::filesystem::path& config_path) {
    ServerConfig config;

    auto full = core::Config::Load(config_path);

    config.host = full.server.host;
    config.port = full.server.port;
    config.tls_enabled = full.server.tls_enabled;
    config.cert_file = full.server.cert_file;
    config.key_file = full.server.key_file;
    config.request_timeout_ms = 30000;

    Logger::Get().Info("Loaded config from: " + config_path.string());
    Logger::Get().Info("Server: " + config.host + ":" + std::to_string(config.port));
    Logger::Get().Info("TLS: " + std::string(config.tls_enabled ? "enabled" : "disabled"));
    Logger::Get().Info("Model path: " + full.model.path);
    Logger::Get().Info("Precision: " + full.model.precision);
    Logger::Get().Info("GPU layers: " + std::to_string(full.model.n_gpu_layers));
    Logger::Get().Info("Context size: " + std::to_string(full.model.context_size));

    return config;
}

std::filesystem::path GetDefaultConfigPath() {
    std::filesystem::path base = std::filesystem::current_path();
    return base / "config" / "gateway.yml";
}

} // namespace inferdeck::gateway
