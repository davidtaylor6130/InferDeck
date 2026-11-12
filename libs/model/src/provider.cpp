#include "model/provider.hpp"

#include "foundation/logging.hpp"

namespace inferdeck::model {

void install_providers(std::span<const ProviderDescriptor> providers,
                       ModelRegistry& registry,
                       const std::vector<ModelInfo>& models)
{
    for (const ProviderDescriptor& provider : providers)
    {
        const std::string& name = provider.contract.name;
        runtime_contracts().register_runtime(provider.contract);
        if (!provider.factory)
        {
            continue;
        }
        if (provider.initialize)
        {
            const foundation::Result<void> initialized = provider.initialize(models);
            if (!initialized)
            {
                foundation::LOG_ERROR("provider_init_failed", "runtime={} origin={} error={}",
                                      name, provider.origin, initialized.error().message);
                continue;
            }
        }
        if (registry.has_factory(name))
        {
            foundation::LOG_WARN("provider_overridden", "runtime={} replaced_by={}",
                                 name, provider.origin);
        }
        registry.register_factory(name, provider.factory);
        foundation::LOG_INFO("provider_registered", "runtime={} origin={} version={}",
                             name, provider.origin, provider.version);
    }
}

}
