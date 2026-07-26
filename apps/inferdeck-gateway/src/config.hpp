#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

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
    int vram_budget_mb{0};
    int vram_safety_margin_mb{1024};
    int max_queue_size{128};
    model::SamplingConfig sampling{};  // global sampler defaults (issue #42)
    std::map<std::string, std::string> anthropic_model_aliases{};
    std::string model_store_root{"models/store"};
    std::string model_store_hf_token{};
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

inline foundation::Result<void> validate_sampling_node(
    const YAML::Node& sampling, const std::string& location) {
    if (!sampling) return foundation::Ok();
    if (!sampling.IsMap()) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     location + " must be a mapping");
    }
    const auto finite_between = [&](const char* key, float minimum, float maximum) {
        if (!sampling[key]) return true;
        const float value = sampling[key].as<float>();
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    if (!finite_between("temperature", 0.0f, 2.0f) ||
        !finite_between("top_p", 0.0f, 1.0f) ||
        !finite_between("min_p", 0.0f, 1.0f) ||
        !finite_between("repeat_penalty", 0.01f, 100.0f) ||
        !finite_between("dry_multiplier", 0.0f, 100.0f) ||
        !finite_between("dry_base", 0.01f, 100.0f)) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     location + " values are outside supported ranges");
    }
    for (const char* key : {"top_k", "dry_allowed_length"}) {
        if (sampling[key] && sampling[key].as<int>() < 0) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         location + "." + key + " cannot be negative");
        }
    }
    for (const char* key : {"repeat_last_n", "dry_penalty_last_n"}) {
        if (sampling[key] && sampling[key].as<int>() < -1) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         location + "." + key + " cannot be less than -1");
        }
    }
    if (sampling["dry_seq_breakers"] && !sampling["dry_seq_breakers"].IsSequence()) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     location + ".dry_seq_breakers must be a sequence");
    }
    return foundation::Ok();
}

inline foundation::Result<void> validate_config_node(const YAML::Node& root) {
    try {
        if (!root || !root.IsMap()) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         "configuration root must be a mapping");
        }
        if (root["server"] && root["server"]["port"]) {
            const int port = root["server"]["port"].as<int>();
            if (port < 1 || port > 65535) {
                return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                             "server.port must be between 1 and 65535");
            }
        }
        if (root["observability"] && root["observability"]["telemetry_poll_ms"] &&
            root["observability"]["telemetry_poll_ms"].as<int>() < 10) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         "observability.telemetry_poll_ms must be at least 10");
        }
        if (root["auth"] && root["auth"]["required"] &&
            root["auth"]["required"].as<bool>() &&
            (!root["auth"]["token"] || root["auth"]["token"].as<std::string>().empty())) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         "auth.token is required when authentication is enabled");
        }
        if (root["gateway"]) {
            const auto& gateway = root["gateway"];
            for (const char* key : {"n_batch", "n_ubatch", "max_queue_size"}) {
                if (gateway[key] && gateway[key].as<int>() < 1) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 std::string("gateway.") + key + " must be positive");
                }
            }
            if (gateway["n_batch"] && gateway["n_ubatch"] &&
                gateway["n_ubatch"].as<int>() > gateway["n_batch"].as<int>()) {
                return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                             "gateway.n_ubatch cannot exceed gateway.n_batch");
            }
            for (const char* key : {"vram_budget_mb", "vram_safety_margin_mb"}) {
                if (gateway[key] && gateway[key].as<int>() < 0) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 std::string("gateway.") + key + " cannot be negative");
                }
            }
            const std::unordered_set<std::string> cache_types{
                "f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "q5_0", "q5_1", "iq4_nl"};
            for (const char* key : {"cache_type_k", "cache_type_v"}) {
                if (gateway[key] && !cache_types.contains(gateway[key].as<std::string>())) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 std::string("gateway.") + key + " is unsupported");
                }
            }
            if (gateway["flash_attn"]) {
                const auto value = gateway["flash_attn"].as<std::string>();
                if (value != "auto" && value != "on" && value != "off") {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "gateway.flash_attn must be auto, on, or off");
                }
            }
            auto sampling = validate_sampling_node(gateway["sampling"], "gateway.sampling");
            if (!sampling) return sampling;
        }
        if (root["model_store"] && root["model_store"]["root"] &&
            root["model_store"]["root"].as<std::string>().empty()) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         "model_store.root cannot be empty");
        }
        std::unordered_set<std::string> names;
        if (root["model_registry"]) {
            if (!root["model_registry"].IsSequence()) {
                return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                             "model_registry must be a sequence");
            }
            for (const auto& entry : root["model_registry"]) {
                if (!entry["name"] || entry["name"].as<std::string>().empty()) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "every model requires a name");
                }
                const std::string name = entry["name"].as<std::string>();
                if (!names.insert(name).second) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "duplicate model name: " + name);
                }
                const std::string runtime = entry["runtime"]
                    ? entry["runtime"].as<std::string>() : "llama_cpp";
                const std::unordered_set<std::string> runtimes{
                    "llama_cpp", "stable_diffusion_cpp", "whisper_cpp",
                    "sherpa_onnx", "windows_sapi"};
                if (!runtimes.contains(runtime)) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "unsupported model runtime for: " + name);
                }
                const std::string modality = entry["modality"]
                    ? entry["modality"].as<std::string>() : "text";
                const std::unordered_set<std::string> modalities{
                    "text", "embedding", "image", "audio_speech", "audio_transcription"};
                if (!modalities.contains(modality)) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "unsupported model modality for: " + name);
                }
                const int slots = entry["n_slots"] ? entry["n_slots"].as<int>() : 2;
                const int minimum = entry["min_slots"] ? entry["min_slots"].as<int>() : 1;
                if (minimum < 1 || slots < minimum) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "invalid slot bounds for model: " + name);
                }
                if (entry["context_size"] && entry["context_size"].as<int>() < 1) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "model context_size must be positive: " + name);
                }
                for (const char* key : {"vram_required_mb", "vram_fixed_mb",
                                        "vram_per_slot_mb"}) {
                    if (entry[key] && entry[key].as<int>() < 0) {
                        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                     "model " + std::string(key) +
                                                     " cannot be negative: " + name);
                    }
                }
                if (runtime == "llama_cpp" &&
                    (!entry["gguf_path"] || entry["gguf_path"].as<std::string>().empty())) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "llama_cpp model requires gguf_path: " + name);
                }
                if (runtime != "llama_cpp" && runtime != "windows_sapi" &&
                    (!entry["artifacts"] || !entry["artifacts"].IsMap() ||
                     entry["artifacts"].size() == 0)) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "native model requires artifacts: " + name);
                }
                const bool modality_matches =
                    (runtime == "llama_cpp" &&
                     (modality == "text" || modality == "embedding")) ||
                    (runtime == "stable_diffusion_cpp" && modality == "image") ||
                    (runtime == "whisper_cpp" && modality == "audio_transcription") ||
                    (runtime == "sherpa_onnx" &&
                     (modality == "audio_speech" ||
                      modality == "audio_transcription")) ||
                    (runtime == "windows_sapi" && modality == "audio_speech");
                if (!modality_matches) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "runtime and modality do not match for: " + name);
                }
                if (entry["has_vision"] && entry["has_vision"].as<bool>()) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "vision input is not implemented for: " + name);
                }
                auto sampling = validate_sampling_node(
                    entry["sampling"], "model_registry." + name + ".sampling");
                if (!sampling) return sampling;
            }
        }
        if (root["default_model"] && !root["default_model"].as<std::string>().empty() &&
            !names.contains(root["default_model"].as<std::string>())) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         "default_model must exist in model_registry");
        }
        return foundation::Ok();
    } catch (const std::exception& error) {
        return foundation::Err<void>(foundation::ErrorCode::ParseError, error.what());
    }
}

inline foundation::Result<void> validate_config_text(const std::string& text) {
    try {
        return validate_config_node(YAML::Load(text));
    } catch (const std::exception& error) {
        return foundation::Err<void>(foundation::ErrorCode::ParseError, error.what());
    }
}

inline GatewayConfig load_config(const std::filesystem::path& path) {
    GatewayConfig cfg;
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("configuration file not found: " + path.string());
    }
    YAML::Node root = YAML::LoadFile(path.string());
    auto valid = validate_config_node(root);
    if (!valid) throw std::runtime_error(valid.error().message);
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
    if (root["model_store"]) {
        const auto& store = root["model_store"];
        if (store["root"]) cfg.model_store_root = store["root"].as<std::string>();
        if (store["hf_token"]) cfg.model_store_hf_token = store["hf_token"].as<std::string>();
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
        if (g["vram_budget_mb"]) cfg.vram_budget_mb = g["vram_budget_mb"].as<int>();
        if (g["vram_safety_margin_mb"]) cfg.vram_safety_margin_mb = g["vram_safety_margin_mb"].as<int>();
        if (g["max_queue_size"]) cfg.max_queue_size = g["max_queue_size"].as<int>();
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
            info.runtime = m["runtime"] ? m["runtime"].as<std::string>() : "llama_cpp";
            info.modality = m["modality"] ? m["modality"].as<std::string>() : "text";
            if (info.modality == "image") info.capabilities = {"image_generation"};
            else if (info.modality == "audio_speech") info.capabilities = {"audio_speech"};
            else if (info.modality == "audio_transcription") info.capabilities = {"audio_transcription"};
            else if (info.modality == "embedding") info.capabilities = {"embeddings"};
            if (m["capabilities"] && m["capabilities"].IsSequence()) {
                info.capabilities.clear();
                for (const auto& capability : m["capabilities"]) {
                    info.capabilities.push_back(capability.as<std::string>());
                }
            }
            if (m["gguf_path"]) info.gguf_path = m["gguf_path"].as<std::string>();
            if (m["artifacts"] && m["artifacts"].IsMap()) {
                for (const auto& artifact : m["artifacts"]) {
                    info.artifacts[artifact.first.as<std::string>()] = artifact.second.as<std::string>();
                }
            }
            if (m["mmproj_path"] && !m["mmproj_path"].IsNull()) {
                info.mmproj_path = m["mmproj_path"].as<std::string>();
            }
            info.n_slots = m["n_slots"] ? m["n_slots"].as<int>() : 2;
            info.min_slots = m["min_slots"] ? m["min_slots"].as<int>() : 1;
            info.vram_required_mb =
                m["vram_required_mb"] ? m["vram_required_mb"].as<int>() : 0;
            info.vram_fixed_mb = m["vram_fixed_mb"] ? m["vram_fixed_mb"].as<int>() : 0;
            info.vram_per_slot_mb = m["vram_per_slot_mb"] ? m["vram_per_slot_mb"].as<int>() : 0;
            info.context_size =
                m["context_size"] ? m["context_size"].as<int>() : 65536;
            if (m["n_gpu_layers"] && !m["n_gpu_layers"].IsNull()) {
                info.n_gpu_layers = m["n_gpu_layers"].as<int>();
            }
            info.has_vision = m["has_vision"] ? m["has_vision"].as<bool>() : false;
            info.reasoning_format = m["reasoning_format"] ? m["reasoning_format"].as<std::string>() : "";
            info.chat_template_path = m["chat_template_path"] ? m["chat_template_path"].as<std::string>() : "";
            // Per-model sampling overrides inherit the global block, then apply
            // any keys present in this entry (issue #42).
            info.sampling = cfg.sampling;
            if (m["sampling"]) parse_sampling(m["sampling"], info.sampling);
            cfg.models.push_back(std::move(info));
        }
    }
    return cfg;
}

inline std::filesystem::path active_config_path_for(
    const std::filesystem::path& base_path) {
    const auto extension = base_path.extension().string();
    return base_path.parent_path() /
        (base_path.stem().string() + ".active" + extension);
}

struct ConfigLoadSelection {
    GatewayConfig config;
    std::filesystem::path loaded_path;
    std::filesystem::path active_path;
    bool using_active{false};
    std::string fallback_reason;
};

inline ConfigLoadSelection load_config_with_active(
    const std::filesystem::path& base_path) {
    ConfigLoadSelection selection;
    selection.active_path = active_config_path_for(base_path);
    if (std::filesystem::exists(selection.active_path)) {
        try {
            selection.config = load_config(selection.active_path);
            selection.loaded_path = selection.active_path;
            selection.using_active = true;
            return selection;
        } catch (const std::exception& error) {
            selection.fallback_reason = error.what();
        }
    }
    selection.config = load_config(base_path);
    selection.loaded_path = base_path;
    return selection;
}

} // namespace inferdeck::gateway
