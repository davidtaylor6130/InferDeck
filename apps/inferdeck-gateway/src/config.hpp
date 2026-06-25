#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
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
    bool auto_swap{true};
    int n_batch{512};
    int n_ubatch{512};
    bool use_mmap{false};
    bool use_mlock{false};
    std::optional<int> n_gpu_layers{};
    std::string flash_attn{"auto"};
    bool kv_offload{true};
    bool op_offload{true};
    std::string cache_type_k{"q8_0"};
    std::string cache_type_v{"q8_0"};
    bool swa_full{false};
    bool truncate_prompt{true};
    model::SamplingConfig sampling{};  // global sampler defaults (issue #42)
    std::map<std::string, std::string> anthropic_model_aliases{};
};

// Overlay any sampler keys present in `node` onto `s` (keys left unspecified
// keep their current value, so per-model blocks inherit the global defaults).
inline void parse_sampling(const YAML::Node& node, model::SamplingConfig& s) {
    if (!node || !node.IsMap()) return;
    if (node["temperature"]) s.temperature = node["temperature"].as<float>();
    if (node["top_p"]) s.top_p = node["top_p"].as<float>();
    if (node["top_k"]) s.top_k = node["top_k"].as<int>();
    if (node["min_p"]) s.min_p = node["min_p"].as<float>();
    if (node["repeat_penalty"]) s.repeat_penalty = node["repeat_penalty"].as<float>();
    if (node["repeat_last_n"]) s.repeat_last_n = node["repeat_last_n"].as<int>();
    if (node["dry_multiplier"]) s.dry_multiplier = node["dry_multiplier"].as<float>();
    if (node["dry_base"]) s.dry_base = node["dry_base"].as<float>();
    if (node["dry_allowed_length"]) s.dry_allowed_length = node["dry_allowed_length"].as<int>();
    if (node["dry_penalty_last_n"]) s.dry_penalty_last_n = node["dry_penalty_last_n"].as<int>();
    if (node["dry_seq_breakers"] && node["dry_seq_breakers"].IsSequence()) {
        s.dry_seq_breakers.clear();
        for (const auto& b : node["dry_seq_breakers"])
            s.dry_seq_breakers.push_back(b.as<std::string>());
    }
}

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
    if (root["gateway"]) {
        const auto& g = root["gateway"];
        if (g["auto_swap"]) cfg.auto_swap = g["auto_swap"].as<bool>();
        if (g["n_batch"]) cfg.n_batch = g["n_batch"].as<int>();
        if (g["n_ubatch"]) cfg.n_ubatch = g["n_ubatch"].as<int>();
        if (g["use_mmap"]) cfg.use_mmap = g["use_mmap"].as<bool>();
        if (g["use_mlock"]) cfg.use_mlock = g["use_mlock"].as<bool>();
        if (g["n_gpu_layers"] && !g["n_gpu_layers"].IsNull()) {
            cfg.n_gpu_layers = g["n_gpu_layers"].as<int>();
        }
        if (g["flash_attn"]) cfg.flash_attn = g["flash_attn"].as<std::string>();
        if (g["kv_offload"]) cfg.kv_offload = g["kv_offload"].as<bool>();
        if (g["op_offload"]) cfg.op_offload = g["op_offload"].as<bool>();
        if (g["cache_type_k"]) cfg.cache_type_k = g["cache_type_k"].as<std::string>();
        if (g["cache_type_v"]) cfg.cache_type_v = g["cache_type_v"].as<std::string>();
        if (g["swa_full"]) cfg.swa_full = g["swa_full"].as<bool>();
        if (g["truncate_prompt"]) cfg.truncate_prompt = g["truncate_prompt"].as<bool>();
        if (g["sampling"]) parse_sampling(g["sampling"], cfg.sampling);
    }
    if (root["anthropic"] && root["anthropic"]["model_aliases"] &&
        root["anthropic"]["model_aliases"].IsMap()) {
        for (const auto& kv : root["anthropic"]["model_aliases"]) {
            cfg.anthropic_model_aliases[kv.first.as<std::string>()] =
                kv.second.as<std::string>();
        }
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
            if (m["n_gpu_layers"] && !m["n_gpu_layers"].IsNull()) {
                info.n_gpu_layers = m["n_gpu_layers"].as<int>();
            }
            info.has_vision = m["has_vision"] ? m["has_vision"].as<bool>() : false;
            info.reasoning_format = m["reasoning_format"] ? m["reasoning_format"].as<std::string>() : "";
            // Per-model sampling overrides inherit the global block, then apply
            // any keys present in this entry (issue #42).
            info.sampling = cfg.sampling;
            if (m["sampling"]) parse_sampling(m["sampling"], info.sampling);
            cfg.models.push_back(std::move(info));
        }
    }
    return cfg;
}

} // namespace inferdeck::gateway
