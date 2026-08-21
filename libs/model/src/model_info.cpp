#include "model/model_info.hpp"

#include <algorithm>

namespace inferdeck::model {

std::string to_string(ModelRole value) {
    switch (value) {
        case ModelRole::Conversation: return "conversation";
        case ModelRole::Helper: return "helper";
        case ModelRole::Media: return "media";
        case ModelRole::Embedding: return "embedding";
        case ModelRole::Maintenance: return "maintenance";
    }
    return "conversation";
}

std::string to_string(ModelCompute value) {
    switch (value) {
        case ModelCompute::Cpu: return "cpu";
        case ModelCompute::VulkanGpu: return "vulkan_gpu";
        case ModelCompute::CudaGpu: return "cuda_gpu";
        case ModelCompute::RocmGpu: return "rocm_gpu";
        case ModelCompute::Mixed: return "mixed";
    }
    return "cpu";
}

std::string to_string(ResidencyPolicy value) {
    switch (value) {
        case ResidencyPolicy::Always: return "always";
        case ResidencyPolicy::Managed: return "managed";
        case ResidencyPolicy::OnDemand: return "on_demand";
    }
    return "managed";
}

std::optional<ModelRole> parse_model_role(const std::string& value) {
    if (value == "conversation") return ModelRole::Conversation;
    if (value == "helper") return ModelRole::Helper;
    if (value == "media") return ModelRole::Media;
    if (value == "embedding") return ModelRole::Embedding;
    if (value == "maintenance") return ModelRole::Maintenance;
    return std::nullopt;
}

std::optional<ModelCompute> parse_model_compute(const std::string& value) {
    if (value == "cpu") return ModelCompute::Cpu;
    if (value == "vulkan_gpu") return ModelCompute::VulkanGpu;
    if (value == "cuda_gpu") return ModelCompute::CudaGpu;
    if (value == "rocm_gpu") return ModelCompute::RocmGpu;
    if (value == "mixed") return ModelCompute::Mixed;
    return std::nullopt;
}

std::optional<ResidencyPolicy> parse_residency_policy(
    const std::string& value) {
    if (value == "always") return ResidencyPolicy::Always;
    if (value == "managed") return ResidencyPolicy::Managed;
    if (value == "on_demand") return ResidencyPolicy::OnDemand;
    return std::nullopt;
}

void normalize_model_resources(ModelInfo& info) {
    if (!info.resource_metadata_explicit) {
        if (info.supports("audio_speech") ||
            info.supports("audio_transcription") ||
            info.supports("image_generation")) {
            info.role = ModelRole::Media;
        } else if (info.supports("embeddings")) {
            info.role = ModelRole::Embedding;
        } else if (info.modality == "text") {
            info.role = ModelRole::Conversation;
        } else if (info.modality == "embedding") {
            info.role = ModelRole::Embedding;
        } else {
            info.role = ModelRole::Media;
        }
        const bool cpu_native =
            info.runtime == "whisper_cpp" ||
            info.runtime == "sherpa_onnx" ||
            info.runtime == "windows_sapi";
        info.compute = cpu_native ? ModelCompute::Cpu
                                  : ModelCompute::VulkanGpu;
        info.residency =
            cpu_native ? ResidencyPolicy::Always : ResidencyPolicy::Managed;
        info.eviction_eligible = info.residency != ResidencyPolicy::Always;
        info.admission_pool = info.name;
        if (info.modality == "audio_speech" &&
            !info.supports("audio_speech")) {
            info.capabilities = {"audio_speech"};
        } else if (info.modality == "audio_transcription" &&
                   !info.supports("audio_transcription")) {
            info.capabilities = {"audio_transcription"};
        } else if (info.modality == "image" &&
                   !info.supports("image_generation")) {
            info.capabilities = {"image_generation"};
        } else if (info.modality == "embedding" &&
                   !info.supports("embeddings")) {
            info.capabilities = {"embeddings"};
        }
    }
    if (info.admission_pool.empty()) {
        info.admission_pool = to_string(info.role);
    }
    if (info.concurrency_limit <= 0) {
        info.concurrency_limit = info.n_slots;
    }
    info.memory_required_mb = std::max(0, info.memory_required_mb);
}

std::optional<std::string> validate_model_resources(const ModelInfo& info) {
    if (info.admission_pool.empty()) {
        return "admission_pool must not be empty";
    }
    if (info.concurrency_limit < 1 ||
        info.concurrency_limit > info.n_slots) {
        return "concurrency_limit must be between 1 and n_slots";
    }
    if (info.memory_required_mb < 0) {
        return "memory_required_mb cannot be negative";
    }
    if (info.residency == ResidencyPolicy::Always &&
        info.eviction_eligible) {
        return "always-resident models cannot be eviction eligible";
    }
    if (info.role == ModelRole::Conversation &&
        info.residency == ResidencyPolicy::Always &&
        info.compute == ModelCompute::Cpu) {
        return "CPU conversation models cannot be always resident";
    }
    if (info.role == ModelRole::Helper &&
        info.residency != ResidencyPolicy::Always) {
        return "helper models must be always resident";
    }
    if (info.role == ModelRole::Helper &&
        info.compute != ModelCompute::Cpu) {
        return "helper models must use CPU compute";
    }
    if (info.role == ModelRole::Embedding &&
        !info.supports("embeddings")) {
        return "embedding role requires the embeddings capability";
    }
    return std::nullopt;
}

bool ModelInfo::supports(const std::string& capability) const {
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

} // namespace inferdeck::model
