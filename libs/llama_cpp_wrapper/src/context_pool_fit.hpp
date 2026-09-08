#pragma once

#include <filesystem>

#include "foundation/result.hpp"
#include "llama.h"
#include "model/ibackend.hpp"

namespace inferdeck::llama_wrapper {

[[nodiscard]] foundation::Result<int> fit_context_pool(
    const std::filesystem::path& model_path,
    const llama_model_params& model_params,
    const llama_context_params& context_params,
    bool mtp,
    int minimum_capacity,
    int maximum_capacity,
    int vram_safety_margin_mb,
    const model::LifecycleControl& control);

} // namespace inferdeck::llama_wrapper
