/// @file ConfigLoader.cpp
/// @brief Gateway configuration loader implementation.

#include "config/ConfigLoader.hpp"
#include "core/Logger.hpp"

#include <fstream>
#include <sstream>

namespace inferdeck::gateway {

ServerConfig LoadConfig(const std::filesystem::path& config_path) {
    ServerConfig config;

    std::ifstream file(config_path);
    if (!file.is_open()) {
        config.port = 8080;
        config.tls_enabled = true;
        config.cert_file = "certs/server.crt";
        config.key_file = "certs/server.key";
        return config;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Simple YAML parsing for V1
    // TODO: Replace with proper yaml-cpp integration
    std::istringstream stream(content);
    std::string line;
    std::string current_section;

    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line.back() == ':') {
            current_section = line.substr(0, line.size() - 1);
            continue;
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // Trim
        size_t end = key.find_last_not_of(" \t");
        if (end != std::string::npos) key = key.substr(0, end + 1);

        start = value.find_first_not_of(" \t\"'");
        if (start != std::string::npos) {
            value = value.substr(start);
            end = value.find_last_not_of(" \t\"'");
            if (end != std::string::npos) value = value.substr(0, end + 1);
        }

        std::string dotted_key = current_section.empty() ? key : current_section + "." + key;

        if (dotted_key == "port") config.port = std::stoi(value);
        else if (dotted_key == "tls.enabled") config.tls_enabled = (value == "true");
        else if (dotted_key == "tls.cert_file") config.cert_file = value;
        else if (dotted_key == "tls.key_file") config.key_file = value;
    }

    return config;
}

std::filesystem::path GetDefaultConfigPath() {
    return std::filesystem::current_path() / "config" / "gateway.yml";
}

} // namespace inferdeck::gateway
