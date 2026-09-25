#pragma once

#include <initializer_list>
#include <optional>
#include <map>
#include <string>
#include <cctype>
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
    if (runtime != "vllm_radiance")
    {
        return std::nullopt;
    }

    const std::map<std::string, std::string>::const_iterator selection = artifacts.find("prefill_attention");
    const std::string prefill = selection == artifacts.end() ? "r4d" : selection->second;
    if (prefill != "r4d" && prefill != "r4d_int4" && prefill != "upstream")
    {
        return "vllm_radiance prefill_attention must be r4d, r4d_int4, or upstream";
    }

    const std::map<std::string, std::string>::const_iterator kv_dtype = artifacts.find("kv_cache_dtype");
    if (kv_dtype != artifacts.end() && kv_dtype->second != "auto" &&
        kv_dtype->second != "int4_per_token_head")
    {
        return "vllm_radiance kv_cache_dtype must be auto or int4_per_token_head";
    }
    const bool int4_kv = kv_dtype != artifacts.end() &&
                         kv_dtype->second == "int4_per_token_head";
    if (artifacts.contains("gpu_memory_utilization"))
    {
        return "vllm_radiance gpu_memory_utilization is not a supported profile setting";
    }
    const std::map<std::string, std::string>::const_iterator decode_dll = artifacts.find("decode_dll");
    const std::map<std::string, std::string>::const_iterator decode_digest = artifacts.find("decode_dll_sha256");
    if ((decode_dll == artifacts.end()) != (decode_digest == artifacts.end()))
    {
        return "vllm_radiance decode_dll and decode_dll_sha256 must be configured together";
    }
    if (decode_dll != artifacts.end())
    {
        if (prefill != "r4d_int4")
        {
            return "vllm_radiance decode DLL artifacts require prefill_attention: r4d_int4";
        }
        if (decode_dll->second.empty())
        {
            return "vllm_radiance decode_dll must not be empty";
        }
        if (decode_digest->second.size() != 64)
        {
            return "vllm_radiance decode_dll_sha256 must be a 64-character SHA-256 digest";
        }
        for (const unsigned char ch : decode_digest->second)
        {
            if (!std::isxdigit(ch))
            {
                return "vllm_radiance decode_dll_sha256 must be a SHA-256 hex digest";
            }
        }
    }
    if (prefill == "r4d_int4")
    {
        if (!int4_kv)
        {
            return "vllm_radiance r4d_int4 requires kv_cache_dtype: int4_per_token_head";
        }
        if (decode_dll == artifacts.end())
        {
            return "vllm_radiance r4d_int4 requires decode_dll and decode_dll_sha256";
        }
        for (const char* key : {"prefill_overlay", "prefill_dll"})
        {
            const std::map<std::string, std::string>::const_iterator artifact = artifacts.find(key);
            if (artifact == artifacts.end() || artifact->second.empty())
            {
                return std::string("vllm_radiance r4d_int4 requires artifact: ") + key;
            }
        }
        const std::map<std::string, std::string>::const_iterator digest = artifacts.find("prefill_dll_sha256");
        if (digest == artifacts.end() || digest->second.size() != 64)
        {
            return "vllm_radiance r4d_int4 requires a 64-character prefill_dll_sha256";
        }
        for (const unsigned char ch : digest->second)
        {
            if (!std::isxdigit(ch))
            {
                return "vllm_radiance prefill_dll_sha256 must be a SHA-256 hex digest";
            }
        }
    }
    else if (prefill == "r4d" && int4_kv)
    {
        return "vllm_radiance r4d prefill requires the default BF16 KV cache; use r4d_int4 for INT4 KV";
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
        true,
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
