#include "native_runtimes/runtime_factories.hpp"

#include <algorithm>

namespace inferdeck::native_runtimes {

#ifdef INFERDECK_HAS_STABLE_DIFFUSION_CPP
std::unique_ptr<model::IBackend> make_stable_diffusion_backend(const model::ModelInfo& info);
#endif
#ifdef INFERDECK_HAS_ACESTEP_CPP
std::unique_ptr<model::IBackend> make_ace_step_backend(const model::ModelInfo& info);
#endif
#ifdef INFERDECK_HAS_LTX_VIDEO_CPP
std::unique_ptr<model::IBackend> make_ltx_video_backend(const model::ModelInfo& info);
#endif
#ifdef INFERDECK_HAS_WHISPER_CPP
std::unique_ptr<model::IBackend> make_whisper_backend(const model::ModelInfo& info);
#endif
#ifdef INFERDECK_HAS_SHERPA_ONNX
std::unique_ptr<model::IBackend> make_sherpa_tts_backend(const model::ModelInfo& info);
std::unique_ptr<model::IBackend> make_sherpa_asr_backend(const model::ModelInfo& info);

std::unique_ptr<model::IBackend> make_sherpa_backend(const model::ModelInfo& info) {
    if (info.modality == "audio_transcription" ||
        std::find(info.capabilities.begin(), info.capabilities.end(),
                  "audio_transcription") != info.capabilities.end()) {
        return make_sherpa_asr_backend(info);
    }
    return make_sherpa_tts_backend(info);
}
#endif
#ifdef INFERDECK_HAS_WINDOWS_SAPI
std::unique_ptr<model::IBackend> make_windows_sapi_backend(const model::ModelInfo& info);
#endif

std::vector<model::ProviderDescriptor> providers() {
    std::vector<model::ProviderDescriptor> result;
    const model::RuntimeContractRegistry contracts = model::standard_runtime_contracts();
    [[maybe_unused]] const auto add = [&](const char* runtime, model::ModelRegistry::BackendFactory factory) {
        result.push_back({*contracts.find(runtime), std::move(factory)});
    };
#ifdef INFERDECK_HAS_STABLE_DIFFUSION_CPP
    add("stable_diffusion_cpp", make_stable_diffusion_backend);
#endif
#ifdef INFERDECK_HAS_ACESTEP_CPP
    add("ace_step_cpp", make_ace_step_backend);
#endif
#ifdef INFERDECK_HAS_LTX_VIDEO_CPP
    add("ltx_video_cpp", make_ltx_video_backend);
#endif
#ifdef INFERDECK_HAS_WHISPER_CPP
    add("whisper_cpp", make_whisper_backend);
#endif
#ifdef INFERDECK_HAS_SHERPA_ONNX
    add("sherpa_onnx", make_sherpa_backend);
#endif
#ifdef INFERDECK_HAS_WINDOWS_SAPI
    add("windows_sapi", make_windows_sapi_backend);
#endif
    return result;
}

void register_factories(model::ModelRegistry& registry) {
    for (model::ProviderDescriptor& provider : providers()) {
        registry.register_factory(provider.contract.name, std::move(provider.factory));
    }
}

std::vector<std::string> available_runtimes() {
    std::vector<std::string> result{"llama_cpp"};
    for (const model::ProviderDescriptor& provider : providers()) {
        result.push_back(provider.contract.name);
    }
    return result;
}

}
