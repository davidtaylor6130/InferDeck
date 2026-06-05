#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "foundation/logging.hpp"
#include "model/model_registry.hpp"

namespace inferdeck::gateway {

struct GatewayConfig {
    std::string host{"0.0.0.0"};
    int port{11434};
    std::string log_level{"info"};
    std::string log_file{};
    std::string default_model{};
    std::string state_file{};
    bool auth_required{false};
    std::string auth_token{};
    std::vector<std::string> cors_origins{};
    std::vector<model::ModelInfo> models{};
    std::string stats_db_path{};
    std::string adlx_helper_path{};
    int telemetry_poll_ms{100};
};

inline std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

inline std::filesystem::path default_config_path() {
    namespace fs = std::filesystem;
    fs::path candidates[] = {
        fs::current_path() / "config" / "gateway.yml",
        fs::current_path() / "gateway.yml",
    };
    for (const auto& c : candidates) {
        if (fs::exists(c)) return c;
    }
    return candidates[0];
}

inline GatewayConfig load_config(const std::filesystem::path& path) {
    GatewayConfig cfg;
    if (!std::filesystem::exists(path)) {
        std::cerr << "config not found: " << path << "\n";
        return cfg;
    }
    YAML::Node root = YAML::LoadFile(path.string());
    if (root["server"]) {
        const auto& s = root["server"];
        if (s["host"]) cfg.host = s["host"].as<std::string>();
        if (s["port"]) cfg.port = s["port"].as<int>();
    }
    if (root["logging"]) {
        const auto& l = root["logging"];
        if (l["level"]) cfg.log_level = l["level"].as<std::string>();
        if (l["file"]) cfg.log_file = l["file"].as<std::string>();
    }
    if (root["auth"]) {
        const auto& a = root["auth"];
        if (a["required"]) cfg.auth_required = a["required"].as<bool>();
        if (a["token"]) cfg.auth_token = a["token"].as<std::string>();
    }
    if (root["cors"]) {
        const auto& c = root["cors"];
        if (c["origins"] && c["origins"].IsSequence()) {
            for (const auto& o : c["origins"]) {
                cfg.cors_origins.push_back(o.as<std::string>());
            }
        }
    }
    if (root["state"] && root["state"]["file"]) {
        cfg.state_file = root["state"]["file"].as<std::string>();
    }
    if (root["default_model"]) {
        cfg.default_model = root["default_model"].as<std::string>();
    }
    if (root["observability"]) {
        const auto& o = root["observability"];
        if (o["stats_db"]) cfg.stats_db_path = o["stats_db"].as<std::string>();
        if (o["adlx_helper"]) cfg.adlx_helper_path = o["adlx_helper"].as<std::string>();
        if (o["telemetry_poll_ms"]) cfg.telemetry_poll_ms = o["telemetry_poll_ms"].as<int>();
    }
    if (root["model_registry"] && root["model_registry"].IsSequence()) {
        for (const auto& m : root["model_registry"]) {
            model::ModelInfo info;
            info.name = m["name"].as<std::string>();
            info.family = m["family"] ? m["family"].as<std::string>() : "unknown";
            info.gguf_path = m["gguf_path"].as<std::string>();
            if (m["mmproj_path"] && !m["mmproj_path"].IsNull()) {
                info.mmproj_path = m["mmproj_path"].as<std::string>();
            }
            info.n_slots = m["n_slots"] ? m["n_slots"].as<int>() : 2;
            info.vram_required_mb =
                m["vram_required_mb"] ? m["vram_required_mb"].as<int>() : 0;
            info.context_size =
                m["context_size"] ? m["context_size"].as<int>() : 65536;
            info.has_vision = m["has_vision"] ? m["has_vision"].as<bool>() : false;
            cfg.models.push_back(std::move(info));
        }
    }
    return cfg;
}

} // namespace inferdeck::gateway
