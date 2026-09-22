#pragma once

#include <initializer_list>
#include <optional>
#include <map>
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

inline std::optional<std::string> validate_runtime_artifacts(
    const std::string& runtime,
    const std::map<std::string, std::string>& artifacts)
{
    const std::map<std::string, std::string>::const_iterator selection =
        artifacts.find("prefill_attention");
    if (runtime == "vllm_radiance" && selection != artifacts.end() &&
        selection->second != "r4d" && selection->second != "upstream")
    {
        return "vllm_radiance prefill_attention must be r4d or upstream";
    }
    return std::nullopt;
}

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
        "vllm_radiance",
        {{"text", {"chat_completions", "responses"}}},
        RuntimeArtifactPolicy::ArtifactMap,
        false,
        true,
        false,
    });
    registry.register_runtime(RuntimeContract{
        "stable_diffusion_cpp",
        {{"image", {"image_generation"}}},
        RuntimeArtifactPolicy::ArtifactMap,
    });
    registry.register_runtime(RuntimeContract{
        "ace_step_cpp",
        {{"audio_generation", {"audio_generation"}}},
        RuntimeArtifactPolicy::ArtifactMap,
    });
    registry.register_runtime(RuntimeContract{
        "ltx_video_cpp",
        {{"video", {"video_generation"}}},
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
