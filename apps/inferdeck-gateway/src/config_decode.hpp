#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

#include "config_types.hpp"
#include "config_validation.hpp"

namespace inferdeck::gateway {

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

}
