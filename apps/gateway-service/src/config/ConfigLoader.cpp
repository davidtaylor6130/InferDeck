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
    config.dashboardPort = full.server.port;
    config.apiPort = full.server.api_port;
    config.tls_enabled = full.server.tls_enabled;
    config.cert_file = full.server.cert_file;
    config.key_file = full.server.key_file;
    config.request_timeout_ms = full.server.request_timeout_ms;
    config.model_path = full.model.path;
    config.model_directory = full.model.directory;
    config.precision = full.model.precision;
    config.n_gpu_layers = full.model.n_gpu_layers;
    config.context_size = full.model.context_size;
    config.whisper_enabled = full.whisper.enabled;
    config.whisper_executable = full.whisper.executable;
    config.whisper_model_directory = full.whisper.model_directory;
    config.whisper_model = full.whisper.model;
    config.whisper_backend = full.whisper.backend;
    config.whisper_language = full.whisper.language;
    config.whisper_task = full.whisper.task;
    config.whisper_extra_args = full.whisper.extra_args;

    Logger::Get().Info("Loaded config from: " + config_path.string());
    Logger::Get().Info("Dashboard: " + config.host + ":" + std::to_string(config.dashboardPort));
    Logger::Get().Info("API: " + config.host + ":" + std::to_string(config.apiPort));
    Logger::Get().Info("TLS: " + std::string(config.tls_enabled ? "enabled" : "disabled"));
    Logger::Get().Info("Model path: " + config.model_path);
    Logger::Get().Info("Precision: " + config.precision);
    Logger::Get().Info("GPU layers: " + std::to_string(config.n_gpu_layers));
    Logger::Get().Info("Context size: " + std::to_string(config.context_size));
    Logger::Get().Info("Whisper runtime: " + std::string(config.whisper_enabled ? "enabled" : "disabled"));
    if (!config.whisper_model.empty()) Logger::Get().Info("Whisper model: " + config.whisper_model);

    return config;
}

std::filesystem::path GetDefaultConfigPath() {
    std::filesystem::path base = std::filesystem::current_path();
    return base / "config" / "gateway.yml";
}

} // namespace inferdeck::gateway
