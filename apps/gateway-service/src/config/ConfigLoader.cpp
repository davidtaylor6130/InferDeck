#include "config/ConfigLoader.hpp"
#include "core/Logger.hpp"
#include "core/Config.hpp"
#include <fstream>

using inferdeck::core::Logger;

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
    config.model_path = full.model.path;
    config.precision = full.model.precision;
    config.n_gpu_layers = full.model.n_gpu_layers;
    config.context_size = full.model.context_size;

    Logger::Get().Info("Loaded config from: " + config_path.string());
    Logger::Get().Info("Server: " + config.host + ":" + std::to_string(config.port));
    Logger::Get().Info("TLS: " + std::string(config.tls_enabled ? "enabled" : "disabled"));
    Logger::Get().Info("Model path: " + config.model_path);
    Logger::Get().Info("Precision: " + config.precision);
    Logger::Get().Info("GPU layers: " + std::to_string(config.n_gpu_layers));
    Logger::Get().Info("Context size: " + std::to_string(config.context_size));

    return config;
}

std::filesystem::path GetDefaultConfigPath() {
    std::filesystem::path base = std::filesystem::current_path();
    return base / "config" / "gateway.yml";
}

} // namespace inferdeck::gateway
