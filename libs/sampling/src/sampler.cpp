#include "sampling/sampler.hpp"

#include <common/sampling.h>
#include <llama.h>

#include <cstdio>
#include <utility>

#include "foundation/logging.hpp"

#include "sampling/SamplerParams.hpp"

namespace inferdeck::sampling {

SamplerChain::~SamplerChain() {
    reset();
}

SamplerChain::SamplerChain(SamplerChain&& other) noexcept : sampler_(other.sampler_) {
    other.sampler_ = nullptr;
}

SamplerChain& SamplerChain::operator=(SamplerChain&& other) noexcept {
    if (this != &other) {
        reset();
        sampler_ = other.sampler_;
        other.sampler_ = nullptr;
    }
    return *this;
}

void SamplerChain::reset(common_sampler* s) noexcept {
    if (sampler_) {
        common_sampler_free(sampler_);
        sampler_ = nullptr;
    }
    sampler_ = s;
}

foundation::Result<void> SamplerChain::accept(int32_t token) {
    if (!sampler_) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     "sampler not initialized");
    }
    common_sampler_accept(sampler_, token, true);
    return foundation::Ok();
}

int32_t SamplerChain::sample(llama_context* ctx, int32_t idx, bool grammar_first) {
    if (!sampler_ || !ctx) return -1;
    return common_sampler_sample(sampler_, ctx, idx, grammar_first);
}

namespace {

#define OR(field, fallback) ((p.field) ? *(p.field) : (fallback))

void apply_to_common(const SamplerParams& p, common_params_sampling& out) {
    out.temp        = OR(temperature, 0.8f);
    out.top_k       = OR(top_k, 40);
    out.top_p       = OR(top_p, 0.95f);
    out.min_p       = OR(min_p, 0.05f);
    out.typ_p       = OR(typical_p, 1.0f);
    out.top_n_sigma = OR(top_n_sigma, 0.0f);
    out.xtc_probability = OR(xtc_probability, 0.0f);
    out.xtc_threshold   = OR(xtc_threshold, 0.1f);

    out.penalty_repeat  = OR(repetition_penalty, 1.0f);
    out.penalty_freq    = OR(frequency_penalty, 0.0f);
    out.penalty_present = OR(presence_penalty, 0.0f);
    out.penalty_last_n  = OR(penalty_last_n, 64);

    out.dry_multiplier     = OR(dry_multiplier, 0.0f);
    out.dry_base           = OR(dry_base, 1.75f);
    out.dry_allowed_length = OR(dry_allowed_length, 2);
    out.dry_penalty_last_n = OR(dry_penalty_last_n, -1);

    out.mirostat      = p.mirostat ? static_cast<int>(*p.mirostat) : 0;
    out.mirostat_eta  = OR(mirostat_eta, 0.1f);
    out.mirostat_tau  = OR(mirostat_tau, 5.0f);
    out.adaptive_target = OR(adaptive_target, 1.0f);
    out.adaptive_decay  = OR(adaptive_decay, 0.99f);

    if (p.grammar && !p.grammar->empty()) {
        out.grammar = common_grammar(COMMON_GRAMMAR_TYPE_USER, *p.grammar);
    }

    out.no_perf = p.no_perf.value_or(true);
    out.seed    = p.seed.value_or(LLAMA_DEFAULT_SEED);
}

#undef OR

} // namespace

foundation::Result<SamplerChain> create_sampler_chain(llama_model* model,
                                                      const SamplerParams& params) {
    if (!model) {
        return foundation::Err<SamplerChain>(foundation::ErrorCode::InvalidArgument,
                                             "llama_model is null");
    }

    common_params_sampling sparams;
    apply_to_common(params, sparams);

    common_sampler* raw = common_sampler_init(model, sparams);
    if (!raw) {
        return foundation::Err<SamplerChain>(foundation::ErrorCode::Internal,
                                             "common_sampler_init returned null");
    }

    SamplerChain chain;
    chain.reset(raw);

    foundation::LOG_INFO("sampler_created",
                         "temp={} top_p={} top_k={} min_p={} rep_pen={}",
                         sparams.temp, sparams.top_p, sparams.top_k,
                         sparams.min_p, sparams.penalty_repeat);

    return foundation::Ok<SamplerChain>(std::move(chain));
}

std::string params_to_yaml(const SamplerParams& p) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "temp: %.3f\ntop_p: %.3f\ntop_k: %d\nmin_p: %.3f\n"
        "rep_pen: %.3f\nfreq_pen: %.3f\npres_pen: %.3f\npen_last_n: %d\n"
        "dry_mult: %.3f\nmirostat: %d\n",
        p.temperature.value_or(0.7f),
        p.top_p.value_or(0.8f),
        p.top_k.value_or(20),
        p.min_p.value_or(0.0f),
        p.repetition_penalty.value_or(1.0f),
        p.frequency_penalty.value_or(0.0f),
        p.presence_penalty.value_or(0.0f),
        p.penalty_last_n.value_or(64),
        p.dry_multiplier.value_or(0.0f),
        p.mirostat ? static_cast<int>(*p.mirostat) : 0);
    return std::string(buf);
}

} // namespace inferdeck::sampling
