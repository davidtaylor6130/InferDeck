#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "foundation/result.hpp"
#include "model/model_registry.hpp"
#include "model/runtime_contract.hpp"

namespace inferdeck::model {

/// <summary>
/// Everything the gateway needs to know about one runtime provider.
/// </summary>
/// <remarks>
/// Built-in runtimes and plugin libraries (see docs/providers.md) both describe
/// themselves with this struct and are wired by <see cref="install_providers"/>.
/// The gateway never names a concrete provider type outside its descriptor.
/// </remarks>
struct ProviderDescriptor {
    /// <summary>Runtime name, modalities, and capabilities. The name is what models select with <c>runtime:</c>.</summary>
    RuntimeContract contract;
    /// <summary>Creates one backend per configured model. Empty when the runtime is not linked.</summary>
    ModelRegistry::BackendFactory factory;
    /// <summary>Optional one-time setup, run before any backend is created.</summary>
    std::function<foundation::Result<void>(const std::vector<ModelInfo>&)> initialize;
    /// <summary>"builtin" or the path of the plugin library.</summary>
    std::string origin{"builtin"};
    std::string version;
};

/// <summary>What happened to one descriptor during <see cref="install_providers"/>.</summary>
struct ProviderStatus {
    std::string runtime;
    std::string origin;
    std::string version;
    /// <summary>
    /// "active": serving its runtime name. "overridden": replaced by a later
    /// descriptor. "not_linked": known contract, no factory in this build.
    /// "init_failed": setup returned an error (see <c>error</c>).
    /// </summary>
    std::string state;
    std::string error;
    RuntimeContract contract;
};

/// <summary>
/// Publishes contracts, runs each provider's setup, and registers its factory.
/// A later descriptor replaces an earlier one with the same runtime name, so
/// plugins listed after built-ins override them. Returns one status per
/// descriptor, in order.
/// </summary>
std::vector<ProviderStatus> install_providers(std::span<const ProviderDescriptor> providers,
                       ModelRegistry& registry,
                       const std::vector<ModelInfo>& models);

}
