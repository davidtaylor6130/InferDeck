#include "gateway/gpu_backend.hpp"

#include "model/provider.hpp"

namespace inferdeck::gateway {

std::string compiled_gpu_backend() {
#ifdef INFERDECK_COMPILED_GPU_BACKEND
    return INFERDECK_COMPILED_GPU_BACKEND;
#else
    return "VULKAN";
#endif
}

nlohmann::json gpu_backend_diagnostics() {
    nlohmann::json devices = nlohmann::json::array();
    const auto reply = model::invoke_provider_operation("llama_cpp", "devices", "{}");
    if (reply) {
        const nlohmann::json parsed = nlohmann::json::parse(*reply, nullptr, false);
        if (parsed.is_object() && parsed.contains("devices") && parsed["devices"].is_array()) {
            devices = parsed["devices"];
        }
    }
    const std::size_t count = devices.size();
    return {
        {"compiledBackend", compiled_gpu_backend()},
        {"deviceCount", count},
        {"devices", std::move(devices)},
    };
}

std::optional<std::string> validate_model_compute(
    const model::ModelInfo& info) {
    if (info.runtime != "llama_cpp") return std::nullopt;
    const std::string compiled = compiled_gpu_backend();
    const bool is_hip = compiled == "HIP";
    switch (info.compute) {
        case model::ModelCompute::Cpu:
        case model::ModelCompute::Mixed:
            return std::nullopt;
        case model::ModelCompute::VulkanGpu:
            if (is_hip) {
                return "model '" + info.name +
                       "' requires the Vulkan GPU backend but this build uses HIP";
            }
            return std::nullopt;
        case model::ModelCompute::RocmGpu:
            if (!is_hip) {
                return "model '" + info.name +
                       "' requires the HIP/ROCm GPU backend but this build uses " +
                       compiled;
            }
            return std::nullopt;
        case model::ModelCompute::CudaGpu:
            return "model '" + info.name +
                   "' requires the CUDA GPU backend, which no InferDeck build provides";
    }
    return std::nullopt;
}

}
