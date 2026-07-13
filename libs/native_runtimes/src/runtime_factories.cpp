#include "native_runtimes/runtime_factories.hpp"

namespace inferdeck::native_runtimes {

#ifdef INFERDECK_HAS_STABLE_DIFFUSION_CPP
std::unique_ptr<model::IBackend> make_stable_diffusion_backend(const model::ModelInfo& info);
#endif
#ifdef INFERDECK_HAS_WHISPER_CPP
std::unique_ptr<model::IBackend> make_whisper_backend(const model::ModelInfo& info);
#endif
#ifdef INFERDECK_HAS_SHERPA_ONNX
std::unique_ptr<model::IBackend> make_sherpa_tts_backend(const model::ModelInfo& info);
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
    registry.register_factory("sherpa_onnx", make_sherpa_tts_backend);
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
    return result;
}

}
