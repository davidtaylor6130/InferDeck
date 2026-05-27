/// @file Config.hpp
/// @brief YAML configuration loader for InferDeck.
///
/// Loads and parses the gateway.yml configuration file, providing
/// type-safe access to all settings.

#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <filesystem>

namespace inferdeck::core {

/// Configuration loader for the InferDeck gateway.
///
/// Reads the gateway.yml file and provides access to all settings
/// with type-safe getters and default values.
class Config {
public:
    /// Server configuration settings.
    struct ServerConfig {
        std::string host = "0.0.0.0";
        int port = 8080;
        int api_port = 11434;
        bool tls_enabled = true;
        std::string cert_file = "certs/server.crt";
        std::string key_file = "certs/server.key";
        int request_timeout_ms = 300000;
    };

    /// Model configuration settings.
    struct ModelConfig {
        std::string path;
        std::string directory;
        std::string precision = "auto";
        int n_gpu_layers = -1;
        int context_size = 100000;
        int batch_size = 512;
        bool flash_attn = true;
        std::string cache_type_k = "q8_0";
        std::string cache_type_v = "q8_0";
        std::string split_mode = "none";
        std::string reasoning_format = "";
        int fit_target = 512;
        int parallel = 1;
        bool kv_unified = true;
    };

    /// Whisper speech-to-text runtime settings.
    struct WhisperConfig {
        bool enabled = false;
        std::string executable;
        std::string model_directory;
        std::string model;
        std::string backend = "vulkan";
        std::string language = "auto";
        std::string task = "transcribe";
        std::string extra_args;
    };

    /// GPU configuration settings.
    struct GpuConfig {
        int device_id = 0;
    };

    /// Queue configuration settings.
    struct QueueConfig {
        int worker_threads = 4;
        int max_queue_size = 100;
    };

    /// Logging configuration settings.
    struct LoggingConfig {
        std::string level = "info";
        std::string file = "logs/gateway.log";
    };

    /// Full gateway configuration.
    struct FullConfig {
        ServerConfig server;
        ModelConfig model;
        WhisperConfig whisper;
        GpuConfig gpu;
        QueueConfig queue;
        LoggingConfig logging;
        bool metrics_enabled = true;
        std::string metrics_endpoint = "/inferdeck/metrics";
    };

    /// Load configuration from a YAML file.
    /// @param config_path Path to the gateway.yml file.
    /// @return The loaded configuration, or error if loading fails.
    static FullConfig Load(const std::filesystem::path& config_path);

    /// Load configuration from a string (YAML format).
    /// @param yaml_content The YAML content as a string.
    /// @return The loaded configuration, or error if parsing fails.
    static FullConfig LoadFromString(const std::string& yaml_content);

    /// Save configuration to a YAML file.
    /// @param config_path Path to save the configuration.
    /// @param config The configuration to save.
    static void Save(const std::filesystem::path& config_path, const FullConfig& config);

    /// Get a string value by key path.
    /// @param key The dotted key path (e.g., "server.port").
    /// @return The string value, or empty string if not found.
    static std::string Get(const std::string& key);

    /// Get an integer value by key path.
    /// @param key The dotted key path.
    /// @param default_value The default if not found.
    /// @return The integer value.
    static int GetInt(const std::string& key, int default_value = 0);

    /// Get a boolean value by key path.
    /// @param key The dotted key path.
    /// @param default_value The default if not found.
    /// @return The boolean value.
    static bool GetBool(const std::string& key, bool default_value = false);

private:
    Config();
    ~Config();
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    std::unordered_map<std::string, std::string> flat_config_;
};

} // namespace inferdeck::core
