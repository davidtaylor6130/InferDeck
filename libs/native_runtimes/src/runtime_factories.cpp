#include "native_runtimes/runtime_factories.hpp"

#include <algorithm>

namespace inferdeck::native_runtimes {

#ifdef INFERDECK_HAS_STABLE_DIFFUSION_CPP
std::unique_ptr<model::IBackend> make_stable_diffusion_backend(const model::ModelInfo& info);
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

void register_factories(model::ModelRegistry& registry) {
    (void)registry;
#ifdef INFERDECK_HAS_STABLE_DIFFUSION_CPP
    registry.register_factory("stable_diffusion_cpp", make_stable_diffusion_backend);
#endif
#ifdef INFERDECK_HAS_WHISPER_CPP
    registry.register_factory("whisper_cpp", make_whisper_backend);
#endif
#ifdef INFERDECK_HAS_SHERPA_ONNX
    registry.register_factory("sherpa_onnx", make_sherpa_backend);
#endif
#ifdef INFERDECK_HAS_WINDOWS_SAPI
    registry.register_factory("windows_sapi", make_windows_sapi_backend);
#endif
}

std::vector<std::string> available_runtimes() {
    std::vector<std::string> result{"llama_cpp"};
#ifdef INFERDECK_HAS_STABLE_DIFFUSION_CPP
    result.push_back("stable_diffusion_cpp");
#endif
#ifdef INFERDECK_HAS_WHISPER_CPP
    result.push_back("whisper_cpp");
#endif
#ifdef INFERDECK_HAS_SHERPA_ONNX
    result.push_back("sherpa_onnx");
#endif
#ifdef INFERDECK_HAS_WINDOWS_SAPI
    result.push_back("windows_sapi");
#endif
    return result;
}

}
