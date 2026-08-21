#pragma once

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "foundation/logging.hpp"
#include "model/model_registry.hpp"
#include "config_schema.hpp"
#include "config_types.hpp"

namespace inferdeck::gateway {

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
        auto schema = validate_config_schema(root);
        if (!schema) return schema;
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
        if (root["control"]) {
            const auto& control = root["control"];
            if (!control.IsMap()) {
                return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                             "control must be a mapping");
            }
            const bool allow_remote = control["allow_remote"] &&
                control["allow_remote"].as<bool>();
            const bool allow_data_plane_token = control["allow_data_plane_token"] &&
                control["allow_data_plane_token"].as<bool>();
            const std::string control_token = control["token"]
                ? control["token"].as<std::string>() : std::string{};
            if (control["origins"] && !control["origins"].IsSequence()) {
                return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                             "control.origins must be a sequence");
            }
            if (control["origins"]) {
                for (const auto& origin : control["origins"]) {
                    const auto value = origin.as<std::string>();
                    const bool scheme = value.starts_with("http://") ||
                        value.starts_with("https://");
                    const auto authority = scheme ? value.find("//") + 2 : 0;
                    if (value.empty() || value == "*" || value == "null" || !scheme ||
                        authority >= value.size() ||
                        value.find_first_of("/?#", authority) != std::string::npos ||
                        value.find('@', authority) != std::string::npos ||
                        value.find_first_of(" \t\r\n") != std::string::npos) {
                        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                     "control.origins must contain exact HTTP(S) origins");
                    }
                }
            }
            if (allow_remote) {
                if (control_token.size() < 32 ||
                    control_token.find_first_of(" \t\r\n") != std::string::npos) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "control.token must contain at least 32 non-whitespace characters for remote administration");
                }
                if (!control["origins"] || control["origins"].size() == 0) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "control.origins is required for remote administration");
                }
                if (!allow_data_plane_token && root["auth"] && root["auth"]["token"] &&
                    !root["auth"]["token"].as<std::string>().empty() &&
                    root["auth"]["token"].as<std::string>() == control_token) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "control.token must differ from auth.token unless explicitly shared");
                }
            }
        }
        if (root["compatibility"]) {
            const auto& compatibility = root["compatibility"];
            if (!compatibility.IsMap()) {
                return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                             "compatibility must be a mapping");
            }
            for (const auto& profile : compatibility) {
                const auto name = profile.first.as<std::string>();
                if (name != "openai_derivative" && name != "anthropic") {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "unsupported compatibility profile: " + name);
                }
                if (!profile.second.IsMap()) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "compatibility." + name + " must be a mapping");
                }
                for (const auto& setting : profile.second) {
                    if (setting.first.as<std::string>() != "enabled") {
                        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                     "unsupported compatibility setting: " +
                                                         name + "." +
                                                         setting.first.as<std::string>());
                    }
                }
                if (profile.second["enabled"]) {
                    (void)profile.second["enabled"].as<bool>();
                }
            }
        }
        if (root["gateway"]) {
            const auto& gateway = root["gateway"];
            for (const char* key : {"n_batch", "n_ubatch", "max_queue_size"}) {
                if (gateway[key] && gateway[key].as<int>() < 1) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 std::string("gateway.") + key + " must be positive");
                }
            }
            if (gateway["voice_session_grace_ms"] &&
                (gateway["voice_session_grace_ms"].as<int>() < 1000 ||
                 gateway["voice_session_grace_ms"].as<int>() > 120000)) {
                return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                             "gateway.voice_session_grace_ms must be between 1000 and 120000");
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
        if (root["model_store"] && root["model_store"]["archive_root"] &&
            root["model_store"]["archive_root"].as<std::string>().empty()) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         "model_store.archive_root cannot be empty");
        }
        std::unordered_set<std::string> names;
        std::unordered_map<std::string, int> admission_pool_limits;
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
                for (const char* key : {"prompt_price_per_million", "cached_prompt_price_per_million", "completion_price_per_million"}) {
                    if (!entry[key]) continue;
                    const double value = entry[key].as<double>();
                    if (!std::isfinite(value) || value < 0.0) {
                        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                     "model " + std::string(key) +
                                                     " must be a non-negative number: " + name);
                    }
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
                const std::array resource_keys{
                    "role", "compute", "residency", "admission_pool",
                    "concurrency_limit", "memory_required_mb",
                    "eviction_eligible"};
                const bool has_resource_metadata = std::any_of(
                    resource_keys.begin(), resource_keys.end(),
                    [&entry](const char* key) {
                        return static_cast<bool>(entry[key]);
                    });
                if (has_resource_metadata &&
                    !std::all_of(resource_keys.begin(), resource_keys.end(),
                                 [&entry](const char* key) {
                                     return static_cast<bool>(entry[key]);
                                 })) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "model resource metadata must define role, compute, "
                        "residency, admission_pool, concurrency_limit, "
                        "memory_required_mb, and eviction_eligible: " + name);
                }
                if (has_resource_metadata) {
                    model::ModelInfo resource;
                    resource.name = name;
                    resource.modality = modality;
                    resource.runtime = runtime;
                    resource.n_slots = slots;
                    resource.capabilities.clear();
                    if (entry["capabilities"] &&
                        entry["capabilities"].IsSequence()) {
                        for (const auto& capability : entry["capabilities"]) {
                            resource.capabilities.push_back(
                                capability.as<std::string>());
                        }
                    } else if (modality == "embedding") {
                        resource.capabilities = {"embeddings"};
                    } else if (modality == "image") {
                        resource.capabilities = {"image_generation"};
                    } else if (modality == "audio_speech") {
                        resource.capabilities = {"audio_speech"};
                    } else if (modality == "audio_transcription") {
                        resource.capabilities = {"audio_transcription"};
                    } else {
                        resource.capabilities = {
                            "chat_completions", "responses"};
                    }
                    const auto role = model::parse_model_role(
                        entry["role"].as<std::string>());
                    const auto compute = model::parse_model_compute(
                        entry["compute"].as<std::string>());
                    const auto residency = model::parse_residency_policy(
                        entry["residency"].as<std::string>());
                    if (!role || !compute || !residency) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "unsupported model role, compute, or residency: " +
                                name);
                    }
                    resource.role = *role;
                    resource.compute = *compute;
                    resource.residency = *residency;
                    resource.admission_pool =
                        entry["admission_pool"].as<std::string>();
                    resource.concurrency_limit =
                        entry["concurrency_limit"].as<int>();
                    resource.memory_required_mb =
                        entry["memory_required_mb"].as<int>();
                    resource.eviction_eligible =
                        entry["eviction_eligible"].as<bool>();
                    resource.resource_metadata_explicit = true;
                    if (const auto error =
                            model::validate_model_resources(resource)) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "invalid model resource metadata for " + name +
                                ": " + *error);
                    }
                    const auto [pool, inserted] =
                        admission_pool_limits.emplace(
                            resource.admission_pool,
                            resource.concurrency_limit);
                    if (!inserted &&
                        pool->second != resource.concurrency_limit) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "models in admission_pool " +
                                resource.admission_pool +
                                " must use the same concurrency_limit");
                    }
                }
                if (entry["context_size"] && entry["context_size"].as<int>() < 1) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "model context_size must be positive: " + name);
                }
                const int model_batch = entry["n_batch"]
                    ? entry["n_batch"].as<int>()
                    : (root["gateway"] && root["gateway"]["n_batch"]
                        ? root["gateway"]["n_batch"].as<int>() : 512);
                const int model_ubatch = entry["n_ubatch"]
                    ? entry["n_ubatch"].as<int>()
                    : (root["gateway"] && root["gateway"]["n_ubatch"]
                        ? root["gateway"]["n_ubatch"].as<int>() : 512);
                if (model_batch < 1 || model_ubatch < 1 ||
                    model_ubatch > model_batch) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "invalid model batch settings: " + name);
                }
                const std::unordered_set<std::string> model_cache_types{
                    "f32", "f16", "bf16", "q8_0", "q4_0", "q4_1",
                    "q5_0", "q5_1", "iq4_nl"};
                for (const char* key : {"cache_type_k", "cache_type_v"}) {
                    if (entry[key] &&
                        !model_cache_types.contains(entry[key].as<std::string>())) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "model " + std::string(key) +
                            " is unsupported: " + name);
                    }
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
                if (entry["has_vision"] && entry["has_vision"].as<bool>() &&
                    (runtime != "llama_cpp" ||
                     !entry["mmproj_path"] || entry["mmproj_path"].IsNull() ||
                     entry["mmproj_path"].as<std::string>().empty())) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "vision model requires llama_cpp and mmproj_path: " + name);
                }
                if (entry["reasoning"]) {
                    const auto reasoning = entry["reasoning"];
                    if (!reasoning.IsMap()) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "model reasoning settings must be a map: " + name);
                    }
                    const bool supported = reasoning["supported"]
                        ? reasoning["supported"].as<bool>() : true;
                    if (!supported && reasoning.size() > 1) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "unsupported reasoning model cannot define effort settings: " + name);
                    }
                    if (supported && (runtime != "llama_cpp" || modality != "text")) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "reasoning requires a llama_cpp text model: " + name);
                    }
                    std::unordered_set<std::string> efforts;
                    if (supported &&
                        (!reasoning["efforts"] || !reasoning["efforts"].IsSequence() ||
                         reasoning["efforts"].size() == 0)) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "reasoning efforts must be a non-empty sequence: " + name);
                    }
                    if (reasoning["efforts"]) {
                        for (const auto& effort : reasoning["efforts"]) {
                            const auto value = effort.as<std::string>();
                            if (value.empty() || value == "none" || !efforts.insert(value).second) {
                                return foundation::Err<void>(
                                    foundation::ErrorCode::InvalidArgument,
                                    "reasoning efforts must be unique non-empty tiers: " + name);
                            }
                        }
                    }
                    const auto default_effort = reasoning["default"]
                        ? reasoning["default"].as<std::string>() : std::string{};
                    if (supported && !efforts.contains(default_effort)) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "reasoning default must be a supported effort: " + name);
                    }
                    if (reasoning["aliases"] && !reasoning["aliases"].IsMap()) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "reasoning aliases must be a map: " + name);
                    }
                    if (reasoning["aliases"]) {
                        for (const auto& alias : reasoning["aliases"]) {
                            const auto source = alias.first.as<std::string>();
                            const auto target = alias.second.as<std::string>();
                            if (source.empty() || source == "none" || !efforts.contains(target)) {
                                return foundation::Err<void>(
                                    foundation::ErrorCode::InvalidArgument,
                                    "reasoning alias must map to a supported effort: " + name);
                            }
                        }
                    }
                }
                if (entry["speculative"]) {
                    const auto speculative = entry["speculative"];
                    if (!speculative.IsMap()) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "model speculative settings must be a map: " + name);
                    }
                    const std::string type = speculative["type"]
                        ? speculative["type"].as<std::string>() : "none";
                    if (type != "none" && type != "mtp") {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "unsupported speculative type for model: " + name);
                    }
                    if (type == "mtp" &&
                        (runtime != "llama_cpp" || modality != "text")) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "MTP requires a llama_cpp text model: " + name);
                    }
                    const int draft_tokens = speculative["draft_tokens"]
                        ? speculative["draft_tokens"].as<int>() : 2;
                    if (draft_tokens < 1 || draft_tokens > 4) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "MTP draft_tokens must be between 1 and 4: " + name);
                    }
                    const float p_min = speculative["p_min"]
                        ? speculative["p_min"].as<float>() : 0.0f;
                    if (!std::isfinite(p_min) || p_min < 0.0f || p_min > 1.0f) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "MTP p_min must be between 0 and 1: " + name);
                    }
                    const int max_active = speculative["max_active_requests"]
                        ? speculative["max_active_requests"].as<int>() : 1;
                    if (max_active < 1 || max_active > slots) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "MTP max_active_requests exceeds slot bounds: " + name);
                    }
                }
                auto sampling = validate_sampling_node(
                    entry["sampling"], "model_registry." + name + ".sampling");
                if (!sampling) return sampling;
                if (entry["optimization"] && entry["optimization"]["schedule"]) {
                    const auto schedule = entry["optimization"]["schedule"];
                    const auto valid_time = [](const std::string& value) {
                        if (value.size() != 5 || value[2] != ':') return false;
                        const int hour = std::stoi(value.substr(0, 2));
                        const int minute = std::stoi(value.substr(3, 2));
                        return hour >= 0 && hour < 24 && minute >= 0 && minute < 60;
                    };
                    const auto start = schedule["window_start"]
                        ? schedule["window_start"].as<std::string>() : "03:00";
                    const auto end = schedule["window_end"]
                        ? schedule["window_end"].as<std::string>() : "04:00";
                    if (!valid_time(start) || !valid_time(end) || start >= end) {
                        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                     "invalid optimization schedule window for: " + name);
                    }
                }
            }
        }
        if (root["model_aliases"]) {
            if (!root["model_aliases"].IsSequence()) {
                return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                             "model_aliases must be a sequence");
            }
            std::unordered_set<std::string> aliases;
            for (const auto& alias : root["model_aliases"]) {
                const auto name = alias["name"] ? alias["name"].as<std::string>() : "";
                const auto target = alias["target"] ? alias["target"].as<std::string>() : "";
                if (name.empty() || target.empty()) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "every model alias requires a name and target");
                }
                if (name.size() > 128 ||
                    !std::all_of(name.begin(), name.end(), [](unsigned char character) {
                        return std::isalnum(character) || character == '-' ||
                               character == '_' || character == '.';
                    })) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "invalid model alias name: " + name);
                }
                if (names.contains(name) || !aliases.insert(name).second) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "duplicate or conflicting model alias: " + name);
                }
                if (!names.contains(target)) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "model alias target must be concrete: " + name);
                }
                const int required_context = alias["required_context_size"]
                    ? alias["required_context_size"].as<int>() : 0;
                if (required_context < 0 ||
                    (alias["required_capabilities"] &&
                     !alias["required_capabilities"].IsSequence())) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "invalid model alias contract: " + name);
                }
                YAML::Node target_entry;
                for (std::size_t index = 0; index < root["model_registry"].size(); ++index) {
                    const auto model_entry = root["model_registry"][index];
                    if (model_entry["name"].as<std::string>() == target) {
                        target_entry = model_entry;
                        break;
                    }
                }
                const int target_context = target_entry["context_size"]
                    ? target_entry["context_size"].as<int>() : 65536;
                if (required_context > target_context) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "model alias context contract is incompatible: " + name);
                }
                std::unordered_set<std::string> target_capabilities;
                if (target_entry["capabilities"] && target_entry["capabilities"].IsSequence()) {
                    for (const auto& capability : target_entry["capabilities"]) {
                        target_capabilities.insert(capability.as<std::string>());
                    }
                } else {
                    const auto modality = target_entry["modality"]
                        ? target_entry["modality"].as<std::string>() : "text";
                    if (modality == "embedding") target_capabilities.insert("embeddings");
                    else if (modality == "image") target_capabilities.insert("image_generation");
                    else if (modality == "audio_speech") target_capabilities.insert("audio_speech");
                    else if (modality == "audio_transcription") target_capabilities.insert("audio_transcription");
                    else target_capabilities = {"chat_completions", "responses"};
                }
                if (alias["required_capabilities"]) {
                    for (const auto& capability : alias["required_capabilities"]) {
                        const auto value = capability.as<std::string>();
                        if (value.empty() || !target_capabilities.contains(value)) {
                            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                         "model alias capability contract is incompatible: " + name);
                        }
                    }
                }
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
    if (root["control"]) {
        const auto& control = root["control"];
        if (control["allow_remote"]) {
            cfg.control_allow_remote = control["allow_remote"].as<bool>();
        }
        if (control["allow_data_plane_token"]) {
            cfg.control_allow_data_plane_token =
                control["allow_data_plane_token"].as<bool>();
        }
        if (control["token"]) cfg.control_token = control["token"].as<std::string>();
        if (control["origins"] && control["origins"].IsSequence()) {
            for (const auto& origin : control["origins"]) {
                cfg.control_origins.push_back(origin.as<std::string>());
            }
        }
    }
    if (root["state"] && root["state"]["file"]) {
        cfg.state_file = root["state"]["file"].as<std::string>();
    }
    if (root["model_store"]) {
        const auto& store = root["model_store"];
        if (store["root"]) cfg.model_store_root = store["root"].as<std::string>();
        if (store["archive_root"]) cfg.model_store_archive_root = store["archive_root"].as<std::string>();
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
        if (g["voice_session_grace_ms"]) cfg.voice_session_grace_ms = g["voice_session_grace_ms"].as<int>();
        if (g["sampling"]) parse_sampling(g["sampling"], cfg.sampling);
    }
    if (root["anthropic"] && root["anthropic"]["model_aliases"] &&
        root["anthropic"]["model_aliases"].IsMap()) {
        for (const auto& kv : root["anthropic"]["model_aliases"]) {
            cfg.anthropic_model_aliases[kv.first.as<std::string>()] =
                kv.second.as<std::string>();
        }
    }
    if (root["compatibility"]) {
        const auto& compatibility = root["compatibility"];
        if (compatibility["anthropic"] &&
            compatibility["anthropic"]["enabled"]) {
            cfg.anthropic_compatibility_enabled =
                compatibility["anthropic"]["enabled"].as<bool>();
        }
        if (compatibility["openai_derivative"] &&
            compatibility["openai_derivative"]["enabled"]) {
            cfg.openai_derivative_compatibility_enabled =
                compatibility["openai_derivative"]["enabled"].as<bool>();
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
            if (m["role"]) {
                info.role = *model::parse_model_role(
                    m["role"].as<std::string>());
                info.compute = *model::parse_model_compute(
                    m["compute"].as<std::string>());
                info.residency = *model::parse_residency_policy(
                    m["residency"].as<std::string>());
                info.admission_pool =
                    m["admission_pool"].as<std::string>();
                info.concurrency_limit =
                    m["concurrency_limit"].as<int>();
                info.memory_required_mb =
                    m["memory_required_mb"].as<int>();
                info.eviction_eligible =
                    m["eviction_eligible"].as<bool>();
                info.resource_metadata_explicit = true;
            }
            info.vram_required_mb =
                m["vram_required_mb"] ? m["vram_required_mb"].as<int>() : 0;
            info.vram_fixed_mb = m["vram_fixed_mb"] ? m["vram_fixed_mb"].as<int>() : 0;
            info.vram_per_slot_mb = m["vram_per_slot_mb"] ? m["vram_per_slot_mb"].as<int>() : 0;
            info.context_size =
                m["context_size"] ? m["context_size"].as<int>() : 65536;
            if (m["n_batch"]) info.n_batch = m["n_batch"].as<int>();
            if (m["n_ubatch"]) info.n_ubatch = m["n_ubatch"].as<int>();
            info.cache_type_k =
                m["cache_type_k"] ? m["cache_type_k"].as<std::string>() : "";
            info.cache_type_v =
                m["cache_type_v"] ? m["cache_type_v"].as<std::string>() : "";
            if (m["n_gpu_layers"] && !m["n_gpu_layers"].IsNull()) {
                info.n_gpu_layers = m["n_gpu_layers"].as<int>();
            }
            if (m["speculative"] && m["speculative"].IsMap()) {
                const auto speculative = m["speculative"];
                info.mtp_enabled =
                    speculative["type"] &&
                    speculative["type"].as<std::string>() == "mtp";
                info.mtp_draft_tokens = speculative["draft_tokens"]
                    ? speculative["draft_tokens"].as<int>() : 2;
                info.mtp_p_min = speculative["p_min"]
                    ? speculative["p_min"].as<float>() : 0.0f;
                info.mtp_max_active_requests =
                    speculative["max_active_requests"]
                        ? speculative["max_active_requests"].as<int>() : 1;
            }
            info.has_vision = m["has_vision"] ? m["has_vision"].as<bool>() : false;
            info.reasoning_format = m["reasoning_format"] ? m["reasoning_format"].as<std::string>() : "";
            info.chat_template_path = m["chat_template_path"] ? m["chat_template_path"].as<std::string>() : "";
            if (m["reasoning"] && m["reasoning"].IsMap()) {
                const auto reasoning = m["reasoning"];
                info.reasoning.supported = reasoning["supported"]
                    ? reasoning["supported"].as<bool>() : true;
                if (reasoning["efforts"] && reasoning["efforts"].IsSequence()) {
                    for (const auto& effort : reasoning["efforts"]) {
                        info.reasoning.efforts.push_back(effort.as<std::string>());
                    }
                }
                info.reasoning.default_effort = reasoning["default"]
                    ? reasoning["default"].as<std::string>() : "";
                info.reasoning.none_disables = reasoning["none_disables"]
                    ? reasoning["none_disables"].as<bool>() : false;
                if (reasoning["aliases"] && reasoning["aliases"].IsMap()) {
                    for (const auto& alias : reasoning["aliases"]) {
                        info.reasoning.aliases[alias.first.as<std::string>()] =
                            alias.second.as<std::string>();
                    }
                }
            }
            if (m["prompt_price_per_million"]) {
                info.prompt_price_per_million = m["prompt_price_per_million"].as<double>();
            }
            if (m["cached_prompt_price_per_million"]) {
                info.cached_prompt_price_per_million = m["cached_prompt_price_per_million"].as<double>();
            }
            if (m["completion_price_per_million"]) {
                info.completion_price_per_million = m["completion_price_per_million"].as<double>();
            }
            if (m["optimization"] && m["optimization"].IsMap()) {
                const auto optimization = m["optimization"];
                info.optimization.status = optimization["status"]
                    ? optimization["status"].as<std::string>() : "";
                info.optimization.measured_at = optimization["measured_at"]
                    ? optimization["measured_at"].as<std::string>() : "";
                info.optimization.quality_passes = optimization["quality_passes"]
                    ? optimization["quality_passes"].as<int>() : 0;
                info.optimization.quality_total = optimization["quality_total"]
                    ? optimization["quality_total"].as<int>() : 0;
                info.optimization.single_tokens_per_second =
                    optimization["single_tokens_per_second"]
                        ? optimization["single_tokens_per_second"].as<double>() : 0.0;
                info.optimization.parallel_tokens_per_second =
                    optimization["parallel_tokens_per_second"]
                        ? optimization["parallel_tokens_per_second"].as<double>() : 0.0;
                if (optimization["schedule"] && optimization["schedule"].IsMap()) {
                    const auto schedule = optimization["schedule"];
                    info.optimization.schedule_enabled = schedule["enabled"]
                        ? schedule["enabled"].as<bool>() : false;
                    info.optimization.schedule_window_start = schedule["window_start"]
                        ? schedule["window_start"].as<std::string>() : "03:00";
                    info.optimization.schedule_window_end = schedule["window_end"]
                        ? schedule["window_end"].as<std::string>() : "04:00";
                }
            }
            // Per-model sampling overrides inherit the global block, then apply
            // any keys present in this entry (issue #42).
            info.sampling = cfg.sampling;
            if (m["sampling"]) parse_sampling(m["sampling"], info.sampling);
            cfg.models.push_back(std::move(info));
        }
    }
    if (root["model_aliases"] && root["model_aliases"].IsSequence()) {
        for (const auto& entry : root["model_aliases"]) {
            model::ModelAlias alias;
            alias.name = entry["name"].as<std::string>();
            alias.target = entry["target"].as<std::string>();
            alias.required_context_size = entry["required_context_size"]
                ? entry["required_context_size"].as<int>() : 0;
            if (entry["required_capabilities"] && entry["required_capabilities"].IsSequence()) {
                for (const auto& capability : entry["required_capabilities"]) {
                    alias.required_capabilities.push_back(capability.as<std::string>());
                }
            }
            cfg.model_aliases.push_back(std::move(alias));
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
