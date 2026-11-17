// The embedded-CPython vLLM Radiance runtime as a v2 provider DLL. CPython is
// initialised by init(), which the gateway calls on its startup thread.
#include "provider/sdk.hpp"
#include "vllm_radiance_wrapper/vllm_radiance_model.hpp"

namespace {

using nlohmann::json;

inferdeck::provider::sdk::Definition define()
{
    return {inferdeck::provider::sdk::contract_manifest("vllm_radiance", INFERDECK_PROVIDER_VERSION),
            [](const inferdeck::model::ModelInfo& info, const json&)
            {
                return std::make_unique<inferdeck::vllm_radiance_wrapper::VllmRadianceModel>(info);
            },
            [](const json& host) -> inferdeck::foundation::Result<void>
            {
                for (const json& model : host.value("models", json::array()))
                {
                    const json artifacts = model.value("artifacts", json::object());
                    if (!artifacts.contains("python_root"))
                    {
                        return inferdeck::foundation::Err<void>(inferdeck::foundation::ErrorCode::InvalidArgument,
                            "model " + model.value("name", std::string{}) + " has no python_root artifact");
                    }
                    return inferdeck::vllm_radiance_wrapper::initialize_python_runtime(
                        artifacts["python_root"].get<std::string>());
                }
                return inferdeck::foundation::Ok();
            }};
}

}

INFERDECK_PROVIDER_SDK(define)
