#include "model/ibackend.hpp"
#include "provider/sdk.hpp"

namespace inferdeck::native_runtimes {
std::unique_ptr<model::IBackend> INFERDECK_PROVIDER_FACTORY(const model::ModelInfo& info);
}

namespace {

inferdeck::provider::sdk::Definition define()
{
    return {inferdeck::provider::sdk::contract_manifest(INFERDECK_PROVIDER_RUNTIME, INFERDECK_PROVIDER_VERSION),
            [](const inferdeck::model::ModelInfo& info, const nlohmann::json&)
            {
                return inferdeck::native_runtimes::INFERDECK_PROVIDER_FACTORY(info);
            }};
}

}

INFERDECK_PROVIDER_SDK(define)
