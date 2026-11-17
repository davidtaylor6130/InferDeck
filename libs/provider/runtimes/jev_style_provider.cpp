// The Jev-Style decision runtime as a v2 provider DLL, with its own static
// llama.cpp. See docs/providers.md.
#include "decision_runtime/jev_style.hpp"
#include "llama.h"
#include "provider/sdk.hpp"

namespace {

inferdeck::provider::sdk::Definition define()
{
    return {inferdeck::provider::sdk::contract_manifest("jev_style", INFERDECK_PROVIDER_VERSION),
            [](const inferdeck::model::ModelInfo& info, const nlohmann::json&)
            {
                return inferdeck::decision_runtime::make_jev_style_backend(info);
            },
            [](const nlohmann::json&) -> inferdeck::foundation::Result<void>
            {
                static const bool initialized = (llama_backend_init(), true);
                (void)initialized;
                return inferdeck::foundation::Ok();
            }};
}

}

INFERDECK_PROVIDER_SDK(define)
