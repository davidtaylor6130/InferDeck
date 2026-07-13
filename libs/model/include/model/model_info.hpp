#pragma once

#include <optional>
#include <string>
#include <vector>

namespace inferdeck::model {

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

struct ModelInfo {
    std::string name{};
    std::string family{};
    std::string runtime{"llama_cpp"};
    std::string modality{"text"};
    std::vector<std::string> capabilities{"chat_completions", "responses"};
    std::string gguf_path{};
    std::string mmproj_path{};
    int n_slots{2};
    int min_slots{1};
    int vram_required_mb{0};
    int vram_fixed_mb{0};
    int vram_per_slot_mb{0};
    int context_size{65536};
    std::optional<int> n_gpu_layers{};
    bool has_vision{false};
    std::string reasoning_format{};
    std::string chat_template_path{};
    SamplingConfig sampling{};

    [[nodiscard]] bool supports(const std::string& capability) const;
};

} // namespace inferdeck::model
