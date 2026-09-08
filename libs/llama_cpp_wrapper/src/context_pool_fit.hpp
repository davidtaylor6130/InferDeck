#pragma once

#include <cstddef>
#include <filesystem>

#include "foundation/result.hpp"
#include "llama.h"
#include "model/ibackend.hpp"

namespace inferdeck::llama_wrapper {

[[nodiscard]] foundation::Result<std::size_t> context_pool_device_memory_bytes(
    const llama_context* target_context,
    const llama_context* draft_context);

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
    const llama_context* reclaimable_draft = nullptr);

} // namespace inferdeck::llama_wrapper
