#include "model/provider.hpp"

#include <map>
#include <mutex>

#include "foundation/logging.hpp"

namespace inferdeck::model {

namespace {

using Operation = decltype(ProviderDescriptor::operation);

std::mutex g_operations_mutex;
std::map<std::string, Operation> g_operations;

}

foundation::Result<std::string> invoke_provider_operation(const std::string& runtime,
                                                          const std::string& operation,
                                                          const std::string& request_json)
{
    Operation found;
    {
        std::lock_guard lock(g_operations_mutex);
        const auto entry = g_operations.find(runtime);
        if (entry != g_operations.end())
        {
            found = entry->second;
        }
    }
    if (!found)
    {
        return foundation::Err<std::string>(foundation::ErrorCode::Unavailable,
                                            "no installed " + runtime + " provider offers " + operation);
    }
    return found(operation, request_json);
}

std::vector<ProviderStatus> install_providers(std::span<const ProviderDescriptor> providers,
                                              ModelRegistry& registry,
                                              const std::vector<ModelInfo>& models)
{
    std::vector<ProviderStatus> statuses;
    for (const ProviderDescriptor& provider : providers)
    {
        const std::string& name = provider.contract.name;
        ProviderStatus status{name, provider.origin, provider.version, "active", {}, provider.contract};
        runtime_contracts().register_runtime(provider.contract);
        if (!provider.factory)
        {
            status.state = "not_linked";
            statuses.push_back(std::move(status));
            continue;
        }
        if (provider.initialize)
        {
            const foundation::Result<void> initialized = provider.initialize(models);
            if (!initialized)
            {
                foundation::LOG_ERROR("provider_init_failed", "runtime={} origin={} error={}",
                                      name, provider.origin, initialized.error().message);
                status.state = "init_failed";
                status.error = initialized.error().message;
                statuses.push_back(std::move(status));
                continue;
            }
        }
        for (ProviderStatus& earlier : statuses)
        {
            if (earlier.runtime == name && earlier.state == "active")
            {
                earlier.state = "overridden";
                foundation::LOG_WARN("provider_overridden", "runtime={} replaced_by={}",
                                     name, provider.origin);
            }
        }
        registry.register_factory(name, provider.factory);
        {
            std::lock_guard lock(g_operations_mutex);
            g_operations[name] = provider.operation;
        }
        foundation::LOG_INFO("provider_registered", "runtime={} origin={} version={}",
                             name, provider.origin, provider.version);
        statuses.push_back(std::move(status));
    }
    return statuses;
}

}
