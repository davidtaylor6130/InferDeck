#pragma once

#include <algorithm>
#include <vector>

#include "foundation/logging.hpp"
#include "native_runtimes/runtime_factories.hpp"
#ifdef INFERDECK_VLLM_RADIANCE_ENABLE
#include "vllm_radiance_wrapper/vllm_radiance_model.hpp"
#endif

namespace inferdeck::gateway {

inline void initialize_optional_runtimes(const std::vector<model::ModelInfo>& models)
{
#ifdef INFERDECK_VLLM_RADIANCE_ENABLE
    const auto radiance_model = std::find_if(
        models.begin(), models.end(), [](const model::ModelInfo& info) {
            return info.runtime == "vllm_radiance";
        });
    if (radiance_model != models.end()) {
        const auto python_root = radiance_model->artifacts.find("python_root");
        if (python_root == radiance_model->artifacts.end()) {
            foundation::LOG_ERROR("radiance_python_runtime_init_failed",
                                  "model={} has no python_root artifact", radiance_model->name);
        } else {
            const auto initialized =
                vllm_radiance_wrapper::initialize_python_runtime(python_root->second);
            if (!initialized) {
                foundation::LOG_ERROR("radiance_python_runtime_init_failed",
                                      "model={} error={}", radiance_model->name,
                                      initialized.error().message);
            } else {
                foundation::LOG_INFO("radiance_python_runtime_initialized",
                                     "python_root={}", python_root->second);
            }
        }
    }
#else
    (void)models;
#endif
}

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
