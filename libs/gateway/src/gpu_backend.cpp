#include "gateway/gpu_backend.hpp"

#include "ggml-backend.h"

namespace inferdeck::gateway {

std::string compiled_gpu_backend() {
#ifdef INFERDECK_COMPILED_GPU_BACKEND
    return INFERDECK_COMPILED_GPU_BACKEND;
#elif defined(GGML_BACKEND_DL)
    return "dynamic";
#elif defined(GGML_USE_HIP)
    return "HIP";
#else
    return "VULKAN";
#endif
}

namespace {

std::string device_type_name(enum ggml_backend_dev_type type) {
    switch (type) {
        case GGML_BACKEND_DEVICE_TYPE_CPU: return "cpu";
        case GGML_BACKEND_DEVICE_TYPE_GPU: return "gpu";
        case GGML_BACKEND_DEVICE_TYPE_IGPU: return "igpu";
        case GGML_BACKEND_DEVICE_TYPE_ACCEL: return "accel";
        default: return "unknown";
    }
}

}

nlohmann::json gpu_backend_diagnostics() {
    nlohmann::json devices = nlohmann::json::array();
    const size_t count = ggml_backend_dev_count();
    for (size_t i = 0; i < count; ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        if (dev == nullptr) continue;
        const char* name = ggml_backend_dev_name(dev);
        const char* description = ggml_backend_dev_description(dev);
        devices.push_back({
            {"index", i},
            {"name", name != nullptr ? name : "unknown"},
            {"description", description != nullptr ? description : ""},
            {"type", device_type_name(ggml_backend_dev_type(dev))},
        });
    }
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
