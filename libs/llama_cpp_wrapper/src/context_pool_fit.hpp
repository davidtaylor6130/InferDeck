#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "foundation/result.hpp"
#include "llama.h"
#include "model/ibackend.hpp"

namespace inferdeck::llama_wrapper {

[[nodiscard]] constexpr bool gpu_compute_unavailable(model::ModelCompute compute, std::size_t gpu_device_count) noexcept {
    return compute != model::ModelCompute::Cpu && gpu_device_count == 0;
}

[[nodiscard]] std::size_t gpu_device_count(const llama_model_params& params);

[[nodiscard]] foundation::Result<std::size_t> context_pool_device_memory_bytes(
    const llama_context* target_context,
    const llama_context* draft_context);

[[nodiscard]] foundation::Result<std::size_t> context_pool_kv_bytes(
    const llama_context* context);

[[nodiscard]] int conservative_reclaimable_context_mb(
    std::size_t kv_bytes, std::int64_t capacity, std::int64_t minimum,
    int vram_safety_margin_mb);

[[nodiscard]] foundation::Result<int> fit_context_pool(
    const std::filesystem::path& model_path,
    const llama_model_params& model_params,
    const llama_context_params& context_params,
    bool mtp,
    int minimum_capacity,
    int maximum_capacity,
    int vram_safety_margin_mb,
    const model::LifecycleControl& control,
    const llama_context* reclaimable_target = nullptr,
    const llama_context* reclaimable_draft = nullptr,
    int* automatic_sequence_capacity = nullptr);

} // namespace inferdeck::llama_wrapper
