#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "foundation/result.hpp"
#include "model/provider.hpp"

namespace inferdeck::provider {

/// <summary>
/// Loads one provider library and describes it. The library stays loaded for
/// the life of the process. See docs/providers.md for the ABI.
/// </summary>
foundation::Result<model::ProviderDescriptor> load_plugin_provider(
    const std::filesystem::path& library);

void set_plugin_host_settings(const std::string& gateway_json);

/// <summary>Outcome of scanning a provider directory.</summary>
struct PluginLoadReport
{
    std::vector<model::ProviderDescriptor> providers;
    /// <summary>One "path: reason" line per library that was skipped.</summary>
    std::vector<std::string> rejected;
};

PluginLoadReport load_plugin_providers(const std::filesystem::path& directory);

}
