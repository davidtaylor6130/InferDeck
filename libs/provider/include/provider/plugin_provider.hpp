#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
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

/// <summary>True when a provider error message reports a lost or removed GPU device.</summary>
bool is_fatal_device_error(std::string_view message);

/// <summary>Called once per provider library when a call fails with a fatal device error.</summary>
using FatalDeviceHandler = std::function<void(const std::string& runtime, const std::string& message)>;
void set_fatal_device_handler(FatalDeviceHandler handler);

/// <summary>Outcome of scanning a provider directory.</summary>
struct PluginLoadReport
{
    std::vector<model::ProviderDescriptor> providers;
    /// <summary>One "path: reason" line per library that was skipped.</summary>
    std::vector<std::string> rejected;
};

PluginLoadReport load_plugin_providers(const std::filesystem::path& directory);

}
