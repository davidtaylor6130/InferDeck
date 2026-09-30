#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "model/model_info.hpp"

namespace inferdeck::gateway {

std::string compiled_gpu_backend();

nlohmann::json gpu_backend_diagnostics();

std::optional<std::string> validate_model_compute(
    const model::ModelInfo& info);

}
