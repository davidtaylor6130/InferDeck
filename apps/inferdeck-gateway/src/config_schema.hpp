#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_set>

#include <yaml-cpp/yaml.h>

#include "foundation/result.hpp"

namespace inferdeck::gateway {

inline constexpr int current_config_schema_version = 1;

inline foundation::Result<void> reject_unknown_config_keys(
    const YAML::Node& node, std::string_view path,
    std::initializer_list<std::string_view> allowed) {
    if (!node || !node.IsMap()) return foundation::Ok();
    std::unordered_set<std::string_view> names(allowed);
    for (const auto& entry : node) {
        const auto key = entry.first.as<std::string>();
        if (!names.contains(key)) {
            const auto location = path.empty() ? key : std::string(path) + "." + key;
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "unknown configuration key: " + location);
        }
    }
    return foundation::Ok();
}

inline foundation::Result<void> validate_config_schema(const YAML::Node& root) {
    if (!root || !root.IsMap()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "configuration root must be a mapping");
    }
    if (root["schema_version"]) {
        const auto version = root["schema_version"].as<int>();
        if (version != current_config_schema_version) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "unsupported schema_version: " + std::to_string(version));
        }
    } else if (root["extensions"]) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "schema_version is required when extensions are configured");
    }
    auto result = reject_unknown_config_keys(root, "", {
        "schema_version", "server", "logging", "auth", "control", "cors",
        "state", "model_store", "default_model", "observability", "gateway",
        "compatibility", "anthropic", "model_aliases", "model_registry",
        "extensions"});
    if (!result) return result;
    const auto check = [&](std::string_view name,
                           std::initializer_list<std::string_view> keys) {
        return reject_unknown_config_keys(root[std::string(name)], name, keys);
    };
    if (!(result = check("server", {"host", "port"}))) return result;
    if (!(result = check("logging", {"level", "file"}))) return result;
    if (!(result = check("auth", {"required", "token"}))) return result;
    if (!(result = check("control", {
            "allow_remote", "allow_data_plane_token", "token", "origins"}))) return result;
    if (!(result = check("cors", {"origins"}))) return result;
    if (!(result = check("state", {"file"}))) return result;
    if (!(result = check("model_store", {"root", "archive_root", "hf_token"}))) return result;
    if (!(result = check("observability", {
            "stats_db", "adlx_helper", "telemetry_poll_ms"}))) return result;
    if (!(result = check("gateway", {
            "auto_swap", "n_batch", "n_ubatch", "use_mmap", "use_mlock",
            "n_gpu_layers", "flash_attn", "kv_offload", "op_offload",
            "cache_type_k", "cache_type_v", "swa_full", "truncate_prompt",
            "vram_budget_mb", "vram_safety_margin_mb", "max_queue_size",
            "voice_session_grace_ms", "sampling"}))) return result;
    if (!(result = check("compatibility", {
            "openai_derivative", "anthropic"}))) return result;
    if (!(result = check("anthropic", {"model_aliases"}))) return result;
    const auto compatibility = root["compatibility"];
    if (compatibility && compatibility.IsMap()) {
        for (const auto* name : {"openai_derivative", "anthropic"}) {
            if (!(result = reject_unknown_config_keys(
                    compatibility[name], "compatibility." + std::string(name),
                    {"enabled"}))) return result;
        }
    }
    const auto gateway = root["gateway"];
    if (gateway && !(result = reject_unknown_config_keys(
            gateway["sampling"], "gateway.sampling", {
                "temperature", "top_p", "top_k", "min_p", "repeat_penalty",
                "repeat_last_n", "dry_multiplier", "dry_base",
                "dry_allowed_length", "dry_penalty_last_n",
                "dry_seq_breakers"}))) return result;
    if (root["model_aliases"] && root["model_aliases"].IsSequence()) {
        std::size_t index = 0;
        for (const auto& alias : root["model_aliases"]) {
            if (!(result = reject_unknown_config_keys(
                    alias, "model_aliases[" + std::to_string(index) + "]", {
                        "name", "target", "required_context_size",
                        "required_capabilities"}))) return result;
            ++index;
        }
    }
    if (root["model_registry"] && root["model_registry"].IsSequence()) {
        std::size_t index = 0;
        for (const auto& model : root["model_registry"]) {
            const auto path = "model_registry[" + std::to_string(index) + "]";
            if (!(result = reject_unknown_config_keys(model, path, {
                    "name", "family", "runtime", "modality", "capabilities",
                    "gguf_path", "artifacts", "mmproj_path", "n_slots",
                    "min_slots", "role", "compute", "residency",
                    "admission_pool", "concurrency_limit", "memory_required_mb",
                    "eviction_eligible", "vram_required_mb", "vram_fixed_mb",
                    "vram_per_slot_mb", "context_size", "n_batch", "n_ubatch",
                    "cache_type_k", "cache_type_v", "n_gpu_layers",
                    "speculative", "has_vision", "reasoning_format",
                    "chat_template_path", "reasoning",
                    "prompt_price_per_million",
                    "cached_prompt_price_per_million",
                    "completion_price_per_million", "optimization",
                    "sampling"}))) return result;
            if (!(result = reject_unknown_config_keys(
                    model["speculative"], path + ".speculative", {
                        "type", "draft_tokens", "p_min",
                        "max_active_requests"}))) return result;
            if (!(result = reject_unknown_config_keys(
                    model["reasoning"], path + ".reasoning", {
                        "supported", "efforts", "default", "none_disables",
                        "aliases"}))) return result;
            if (!(result = reject_unknown_config_keys(
                    model["optimization"], path + ".optimization", {
                        "status", "measured_at", "quality_passes",
                        "quality_total", "single_tokens_per_second",
                        "parallel_tokens_per_second", "schedule"}))) return result;
            const auto optimization = model["optimization"];
            if (optimization && !(result = reject_unknown_config_keys(
                    optimization["schedule"], path + ".optimization.schedule",
                    {"enabled", "window_start", "window_end"}))) return result;
            if (!(result = reject_unknown_config_keys(
                    model["sampling"], path + ".sampling", {
                        "temperature", "top_p", "top_k", "min_p",
                        "repeat_penalty", "repeat_last_n", "dry_multiplier",
                        "dry_base", "dry_allowed_length", "dry_penalty_last_n",
                        "dry_seq_breakers"}))) return result;
            ++index;
        }
    }
    return foundation::Ok();
}

}
