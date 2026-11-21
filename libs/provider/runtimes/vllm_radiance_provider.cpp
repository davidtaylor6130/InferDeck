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
                std::string python_root;
                for (const json& model : host.value("models", json::array()))
                {
                    const json artifacts = model.value("artifacts", json::object());
                    const std::string name = model.value("name", std::string{});
                    if (!artifacts.contains("python_root"))
                    {
                        return inferdeck::foundation::Err<void>(inferdeck::foundation::ErrorCode::InvalidArgument,
                            "model " + name + " has no python_root artifact");
                    }
                    const std::string root = artifacts["python_root"].get<std::string>();
                    if (!python_root.empty() && root != python_root)
                    {
                        return inferdeck::foundation::Err<void>(inferdeck::foundation::ErrorCode::InvalidArgument,
                            "model " + name + " uses python_root " + root + " but another model uses " +
                                python_root + "; one embedded Python serves every vllm_radiance model");
                    }
                    python_root = root;
                }
                if (!python_root.empty())
                {
                    return inferdeck::vllm_radiance_wrapper::initialize_python_runtime(python_root);
                }
                return inferdeck::foundation::Ok();
            }};
}

}

INFERDECK_PROVIDER_SDK(define)
