#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

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
    bool control_allow_remote{false};
    bool control_allow_data_plane_token{false};
    std::string control_token{};
    std::vector<std::string> control_origins{};
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
    int voice_session_grace_ms{15000};
    model::SamplingConfig sampling{};
    bool openai_derivative_compatibility_enabled{false};
    std::vector<model::ModelAlias> model_aliases{};
    std::string model_store_root{"models/store"};
    std::string model_store_archive_root{"models/archive"};
    std::string model_store_hf_token{};
};

}
