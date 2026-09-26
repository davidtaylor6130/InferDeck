#pragma once

#include "native_runtimes/runtime_factories.hpp"
#ifdef INFERDECK_VLLM_RADIANCE_ENABLE
#include "vllm_radiance_wrapper/vllm_radiance_model.hpp"
#endif

namespace inferdeck::gateway {

inline void register_optional_runtime_factories(model::ModelRegistry& registry)
{
#ifdef INFERDECK_VLLM_RADIANCE_ENABLE
    registry.register_factory("vllm_radiance", [](const model::ModelInfo& info) -> std::unique_ptr<model::IBackend>
    {
        return std::make_unique<vllm_radiance_wrapper::VllmRadianceModel>(info);
    });
#endif
    native_runtimes::register_factories(registry);
}

}
