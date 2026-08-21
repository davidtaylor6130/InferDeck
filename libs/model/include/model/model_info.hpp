#pragma once

#include <optional>
#include <map>
#include <string>
#include <vector>

namespace inferdeck::model {

enum class ModelRole {
    Conversation,
    Helper,
    Media,
    Embedding,
    Maintenance,
};

enum class ModelCompute {
    Cpu,
    VulkanGpu,
    CudaGpu,
    RocmGpu,
    Mixed,
};

enum class ResidencyPolicy {
    Always,
    Managed,
    OnDemand,
};

std::string to_string(ModelRole value);
std::string to_string(ModelCompute value);
std::string to_string(ResidencyPolicy value);
std::optional<ModelRole> parse_model_role(const std::string& value);
std::optional<ModelCompute> parse_model_compute(const std::string& value);
std::optional<ResidencyPolicy> parse_residency_policy(
    const std::string& value);

struct SamplingConfig {
    float temperature{0.8f};
    float top_p{0.95f};
    int top_k{40};
    float min_p{0.05f};
    float repeat_penalty{1.0f};
    int repeat_last_n{64};
    float dry_multiplier{0.0f};
    float dry_base{1.75f};
    int dry_allowed_length{2};
    int dry_penalty_last_n{-1};
    std::vector<std::string> dry_seq_breakers{"\n", ":", "\"", "*"};
};

struct ProfileOptimizationInfo {
    std::string status{};
    std::string measured_at{};
    int quality_passes{0};
    int quality_total{0};
    double single_tokens_per_second{0.0};
    double parallel_tokens_per_second{0.0};
    bool schedule_enabled{false};
    std::string schedule_window_start{"03:00"};
    std::string schedule_window_end{"04:00"};
};

struct ReasoningConfig {
    bool supported{false};
    std::vector<std::string> efforts{};
    std::string default_effort{};
    bool none_disables{false};
    std::map<std::string, std::string> aliases{};
};

struct ModelAlias {
    std::string name{};
    std::string target{};
    int required_context_size{0};
    std::vector<std::string> required_capabilities{};
};

struct ModelInfo {
    std::string name{};
    std::string family{};
    std::string runtime{"llama_cpp"};
    std::string modality{"text"};
    ModelRole role{ModelRole::Conversation};
    ModelCompute compute{ModelCompute::VulkanGpu};
    ResidencyPolicy residency{ResidencyPolicy::Managed};
    std::string admission_pool{"conversation"};
    int concurrency_limit{0};
    int memory_required_mb{0};
    bool eviction_eligible{true};
    bool resource_metadata_explicit{false};
    std::vector<std::string> capabilities{"chat_completions", "responses"};
    std::string gguf_path{};
    std::string mmproj_path{};
    int n_slots{2};
    int min_slots{1};
    int vram_required_mb{0};
    int vram_fixed_mb{0};
    int vram_per_slot_mb{0};
    int context_size{65536};
    std::optional<int> n_batch{};
    std::optional<int> n_ubatch{};
    std::string cache_type_k{};
    std::string cache_type_v{};
    std::optional<int> n_gpu_layers{};
    bool mtp_enabled{false};
    int mtp_draft_tokens{2};
    float mtp_p_min{0.0f};
    int mtp_max_active_requests{1};
    bool has_vision{false};
    std::string reasoning_format{};
    std::string chat_template_path{};
    std::optional<double> prompt_price_per_million{};
    std::optional<double> cached_prompt_price_per_million{};
    std::optional<double> completion_price_per_million{};
    std::map<std::string, std::string> artifacts{};
    SamplingConfig sampling{};
    ReasoningConfig reasoning{};
    ProfileOptimizationInfo optimization{};

    [[nodiscard]] bool supports(const std::string& capability) const;
};

void normalize_model_resources(ModelInfo& info);
std::optional<std::string> validate_model_resources(const ModelInfo& info);

} // namespace inferdeck::model
