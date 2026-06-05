#include "sampling/family_defaults.hpp"

#include "sampling/family.hpp"

namespace inferdeck::sampling {

bool is_known_family(std::string_view family) {
    return family == kFamilyQwen3 ||
           family == kFamilyQwen3_5 ||
           family == kFamilyQwen3_6 ||
           family == kFamilyQwen3Coder ||
           family == kFamilyQwen3CoderNext;
}

SamplerParams defaults_for_family(std::string_view family) {
    SamplerParams p;

    if (family == kFamilyQwen3Coder || family == kFamilyQwen3CoderNext) {
        p.temperature = 0.6f;
        p.top_p = 0.95f;
        p.top_k = 20;
        p.min_p = 0.0f;
        p.repetition_penalty = 1.0f;
        p.frequency_penalty = 0.0f;
        p.presence_penalty = 0.0f;
        p.dry_multiplier = 0.0f;
        p.penalty_last_n = 256;
        p.mirostat = MirostatMode::Off;
        return p;
    }

    if (family == kFamilyQwen3 || family == kFamilyQwen3_5 || family == kFamilyQwen3_6) {
        p.temperature = 0.7f;
        p.top_p = 0.8f;
        p.top_k = 20;
        p.min_p = 0.0f;
        p.repetition_penalty = 1.05f;
        p.frequency_penalty = 0.0f;
        p.presence_penalty = 0.0f;
        p.dry_multiplier = 0.0f;
        p.penalty_last_n = 64;
        p.mirostat = MirostatMode::Off;
        return p;
    }

    p.temperature = 0.8f;
    p.top_p = 0.95f;
    p.top_k = 40;
    p.min_p = 0.05f;
    p.repetition_penalty = 1.1f;
    p.penalty_last_n = 64;
    return p;
}

SamplerParams apply_user_overrides(SamplerParams base, const SamplerParams& overrides) {
    if (overrides.temperature)            base.temperature = overrides.temperature;
    if (overrides.top_p)                  base.top_p = overrides.top_p;
    if (overrides.top_k)                  base.top_k = overrides.top_k;
    if (overrides.min_p)                  base.min_p = overrides.min_p;
    if (overrides.typical_p)              base.typical_p = overrides.typical_p;
    if (overrides.top_n_sigma)            base.top_n_sigma = overrides.top_n_sigma;
    if (overrides.xtc_probability)        base.xtc_probability = overrides.xtc_probability;
    if (overrides.xtc_threshold)          base.xtc_threshold = overrides.xtc_threshold;

    if (overrides.repetition_penalty)     base.repetition_penalty = overrides.repetition_penalty;
    if (overrides.frequency_penalty)      base.frequency_penalty = overrides.frequency_penalty;
    if (overrides.presence_penalty)       base.presence_penalty = overrides.presence_penalty;
    if (overrides.penalty_last_n)         base.penalty_last_n = overrides.penalty_last_n;

    if (overrides.dry_multiplier)         base.dry_multiplier = overrides.dry_multiplier;
    if (overrides.dry_base)               base.dry_base = overrides.dry_base;
    if (overrides.dry_allowed_length)     base.dry_allowed_length = overrides.dry_allowed_length;
    if (overrides.dry_penalty_last_n)     base.dry_penalty_last_n = overrides.dry_penalty_last_n;
    if (overrides.dry_sequence_breakers)  base.dry_sequence_breakers = overrides.dry_sequence_breakers;

    if (overrides.mirostat)               base.mirostat = overrides.mirostat;
    if (overrides.mirostat_eta)           base.mirostat_eta = overrides.mirostat_eta;
    if (overrides.mirostat_tau)           base.mirostat_tau = overrides.mirostat_tau;
    if (overrides.adaptive_target)        base.adaptive_target = overrides.adaptive_target;
    if (overrides.adaptive_decay)         base.adaptive_decay = overrides.adaptive_decay;

    if (overrides.grammar)                base.grammar = overrides.grammar;
    if (overrides.grammar_lazy)           base.grammar_lazy = overrides.grammar_lazy;
    if (overrides.grammar_triggers)       base.grammar_triggers = overrides.grammar_triggers;

    if (overrides.seed)                   base.seed = overrides.seed;
    if (overrides.no_perf)                base.no_perf = overrides.no_perf;

    return base;
}

} // namespace inferdeck::sampling
