#include <algorithm>

#include "model/ibackend.hpp"
#include "provider/sdk.hpp"

namespace inferdeck::native_runtimes {
std::unique_ptr<model::IBackend> make_sherpa_tts_backend(const model::ModelInfo& info);
std::unique_ptr<model::IBackend> make_sherpa_asr_backend(const model::ModelInfo& info);
}

namespace {

inferdeck::provider::sdk::Definition define()
{
    return {inferdeck::provider::sdk::contract_manifest("sherpa_onnx", INFERDECK_PROVIDER_VERSION),
            [](const inferdeck::model::ModelInfo& info, const nlohmann::json&)
            {
                const bool transcription = info.modality == "audio_transcription" ||
                    std::find(info.capabilities.begin(), info.capabilities.end(), "audio_transcription") !=
                        info.capabilities.end();
                return transcription ? inferdeck::native_runtimes::make_sherpa_asr_backend(info)
                                     : inferdeck::native_runtimes::make_sherpa_tts_backend(info);
            }};
}

}

INFERDECK_PROVIDER_SDK(define)
