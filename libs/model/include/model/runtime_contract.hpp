#pragma once

#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace inferdeck::model {

enum class RuntimeArtifactPolicy {
    Gguf,
    ArtifactMap,
    None,
};

struct RuntimeContract {
    std::string name;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        modality_capabilities;
    RuntimeArtifactPolicy artifact_policy{RuntimeArtifactPolicy::ArtifactMap};
    bool vision{false};
    bool reasoning{false};
    bool speculative{false};
};

class RuntimeContractRegistry {
public:
    void register_runtime(RuntimeContract contract) {
        contracts_.insert_or_assign(contract.name, std::move(contract));
    }

    const RuntimeContract* find(const std::string& name) const {
        const auto found = contracts_.find(name);
        return found == contracts_.end() ? nullptr : &found->second;
    }

    std::optional<std::string> validate(
        const std::string& runtime, const std::string& modality,
        const std::vector<std::string>& capabilities) const {
        const auto* contract = find(runtime);
        if (!contract) return "runtime is not registered: " + runtime;
        const auto supported = contract->modality_capabilities.find(modality);
        if (supported == contract->modality_capabilities.end()) {
            return "runtime " + runtime + " does not support modality " + modality;
        }
        for (const auto& capability : capabilities) {
            if (!supported->second.contains(capability)) {
                return "runtime " + runtime + " modality " + modality +
                    " does not support capability " + capability;
            }
        }
        return std::nullopt;
    }

private:
    std::unordered_map<std::string, RuntimeContract> contracts_;
};

inline RuntimeContractRegistry standard_runtime_contracts() {
    RuntimeContractRegistry registry;
    registry.register_runtime(RuntimeContract{
        "llama_cpp",
        {
            {"text", {"chat_completions", "responses"}},
            {"embedding", {"embeddings"}},
        },
        RuntimeArtifactPolicy::Gguf,
        true,
        true,
        true,
    });
    registry.register_runtime(RuntimeContract{
        "stable_diffusion_cpp",
        {{"image", {"image_generation"}}},
        RuntimeArtifactPolicy::ArtifactMap,
    });
    registry.register_runtime(RuntimeContract{
        "whisper_cpp",
        {{"audio_transcription", {"audio_transcription"}}},
        RuntimeArtifactPolicy::ArtifactMap,
    });
    registry.register_runtime(RuntimeContract{
        "sherpa_onnx",
        {
            {"audio_speech", {"audio_speech"}},
            {"audio_transcription", {"audio_transcription"}},
        },
        RuntimeArtifactPolicy::ArtifactMap,
    });
    registry.register_runtime(RuntimeContract{
        "windows_sapi",
        {{"audio_speech", {"audio_speech"}}},
        RuntimeArtifactPolicy::None,
    });
    return registry;
}

}
