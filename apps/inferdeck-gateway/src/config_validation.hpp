#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <yaml-cpp/yaml.h>

#include "config_schema.hpp"
#include "foundation/result.hpp"
#include "model/model_registry.hpp"
#include "model/runtime_contract.hpp"

namespace inferdeck::gateway {

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
        const auto runtime_contracts = model::standard_runtime_contracts();
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
                const bool cookie_safe = std::all_of(
                    control_token.begin(), control_token.end(), [](unsigned char ch) {
                        return std::isalnum(ch) || ch == '-' || ch == '_' ||
                            ch == '.' || ch == '~';
                    });
                if (control_token.size() < 32 || !cookie_safe) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "control.token must contain at least 32 cookie-safe ASCII characters for remote administration");
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
                if (name != "openai_derivative") {
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
                const auto* runtime_contract = runtime_contracts.find(runtime);
                if (!runtime_contract) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "unregistered model runtime for " + name +
                                                     ": " + runtime);
                }
                const std::string modality = entry["modality"]
                    ? entry["modality"].as<std::string>() : "text";
                std::vector<std::string> capabilities;
                if (entry["capabilities"] && entry["capabilities"].IsSequence()) {
                    for (const auto& capability : entry["capabilities"]) {
                        capabilities.push_back(capability.as<std::string>());
                    }
                } else {
                    const auto supported =
                        runtime_contract->modality_capabilities.find(modality);
                    if (supported != runtime_contract->modality_capabilities.end()) {
                        capabilities.assign(
                            supported->second.begin(), supported->second.end());
                    }
                }
                if (const auto error = runtime_contracts.validate(
                        runtime, modality, capabilities)) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 *error + " for model " + name);
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
                    resource.capabilities = capabilities;
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
                if (runtime_contract->artifact_policy ==
                        model::RuntimeArtifactPolicy::Gguf &&
                    (!entry["gguf_path"] || entry["gguf_path"].as<std::string>().empty())) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "runtime requires gguf_path: " + name);
                }
                if (runtime_contract->artifact_policy ==
                        model::RuntimeArtifactPolicy::ArtifactMap &&
                    (!entry["artifacts"] || !entry["artifacts"].IsMap() ||
                     entry["artifacts"].size() == 0)) {
                    return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                                 "native model requires artifacts: " + name);
                }
                if (entry["has_vision"] && entry["has_vision"].as<bool>() &&
                    (!runtime_contract->vision ||
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
                    if (supported && !runtime_contract->reasoning) {
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
                    if (type == "mtp" && !runtime_contract->speculative) {
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

}
