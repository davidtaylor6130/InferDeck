#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace inferdeck::sampling {

enum class GrammarTriggerType {
    Word,
    Pattern,
    PatternFull,
    Token,
};

struct GrammarTrigger {
    GrammarTriggerType type{GrammarTriggerType::Word};
    std::string value{};
    int32_t token{0};
};

enum class MirostatMode {
    Off = 0,
    V1 = 1,
    V2 = 2,
};

namespace detail {
constexpr float kUnsetFloat = std::numeric_limits<float>::quiet_NaN();
constexpr int32_t kUnsetInt = -1;
constexpr uint32_t kUnsetUint = 0xFFFFFFFFu;
} // namespace detail

struct SamplerParams {
    std::optional<float> temperature{std::nullopt};
    std::optional<float> top_p{std::nullopt};
    std::optional<int32_t> top_k{std::nullopt};
    std::optional<float> min_p{std::nullopt};
    std::optional<float> typical_p{std::nullopt};
    std::optional<float> top_n_sigma{std::nullopt};
    std::optional<float> xtc_probability{std::nullopt};
    std::optional<float> xtc_threshold{std::nullopt};

    std::optional<float> repetition_penalty{std::nullopt};
    std::optional<float> frequency_penalty{std::nullopt};
    std::optional<float> presence_penalty{std::nullopt};
    std::optional<int32_t> penalty_last_n{std::nullopt};

    std::optional<float> dry_multiplier{std::nullopt};
    std::optional<float> dry_base{std::nullopt};
    std::optional<int32_t> dry_allowed_length{std::nullopt};
    std::optional<int32_t> dry_penalty_last_n{std::nullopt};
    std::optional<std::vector<std::string>> dry_sequence_breakers{std::nullopt};

    std::optional<MirostatMode> mirostat{std::nullopt};
    std::optional<float> mirostat_eta{std::nullopt};
    std::optional<float> mirostat_tau{std::nullopt};
    std::optional<float> adaptive_target{std::nullopt};
    std::optional<float> adaptive_decay{std::nullopt};

    std::optional<std::string> grammar{std::nullopt};
    std::optional<bool> grammar_lazy{std::nullopt};
    std::optional<std::vector<GrammarTrigger>> grammar_triggers{std::nullopt};

    std::optional<uint32_t> seed{std::nullopt};
    std::optional<bool> no_perf{std::nullopt};
};

} // namespace inferdeck::sampling
