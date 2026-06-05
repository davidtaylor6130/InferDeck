#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "foundation/result.hpp"
#include "sampling/SamplerParams.hpp"

struct llama_model;
struct llama_context;
struct common_sampler;

namespace inferdeck::sampling {

class SamplerChain {
public:
    SamplerChain() = default;
    ~SamplerChain();
    SamplerChain(const SamplerChain&) = delete;
    SamplerChain& operator=(const SamplerChain&) = delete;
    SamplerChain(SamplerChain&& other) noexcept;
    SamplerChain& operator=(SamplerChain&& other) noexcept;

    [[nodiscard]] common_sampler* raw() const noexcept { return sampler_; }
    [[nodiscard]] explicit operator bool() const noexcept { return sampler_ != nullptr; }

    void reset(common_sampler* s = nullptr) noexcept;

    [[nodiscard]] foundation::Result<void> accept(int32_t token);
    [[nodiscard]] int32_t sample(llama_context* ctx, int32_t idx, bool grammar_first = false);

private:
    common_sampler* sampler_{nullptr};
};

[[nodiscard]] foundation::Result<SamplerChain> create_sampler_chain(
    llama_model* model,
    const SamplerParams& params);

[[nodiscard]] std::string params_to_yaml(const SamplerParams& p);

} // namespace inferdeck::sampling
