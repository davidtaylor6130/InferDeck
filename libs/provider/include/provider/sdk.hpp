#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "foundation/result.hpp"
#include "inferdeck/provider_abi.h"
#include "model/ibackend.hpp"

/// <summary>
/// Turns any C++ IBackend factory into a v2 provider DLL. The SDK forwards
/// every ABI operation to the backend's virtual of the same name, so the
/// runtime behaves exactly as it does when compiled into the gateway.
/// </summary>
/// <example>
/// <code>
/// inferdeck::provider::sdk::Definition define()
/// {
///     return {R"({"runtime":"my_runtime","modalities":{"image":["image_generation"]}})",
///             [](const inferdeck::model::ModelInfo& info, const nlohmann::json&)
///             { return make_my_backend(info); }};
/// }
/// INFERDECK_PROVIDER_SDK(define)
/// </code>
/// </example>
namespace inferdeck::provider::sdk {

struct Definition
{
    /// <summary>The manifest, exactly as documented in docs/providers.md.</summary>
    std::string manifest_json;
    /// <summary>One backend per configured model. <c>gateway</c> is host_json["gateway"] from the last init.</summary>
    std::function<std::unique_ptr<model::IBackend>(const model::ModelInfo& info, const nlohmann::json& gateway)> factory;
    /// <summary>Optional. Runs on the gateway's startup thread with the whole host_json.</summary>
    std::function<foundation::Result<void>(const nlohmann::json& host)> init{};
    /// <summary>Optional. Runs once at process exit.</summary>
    std::function<void()> shutdown{};
    /// <summary>Optional process-level operations (no model instance), e.g. "quantize". Reply JSON or an error.</summary>
    std::function<foundation::Result<nlohmann::json>(const std::string& operation, const nlohmann::json& request)>
        operation{};
};

/// <summary>
/// Manifest JSON for one of InferDeck's standard runtimes, taken from
/// model::standard_runtime_contracts(), so a DLL build of a built-in runtime
/// declares exactly what the built-in does.
/// </summary>
std::string contract_manifest(const std::string& runtime, const std::string& version);

/// <summary>Builds the provider table on first use from <paramref name="define"/>.</summary>
const inferdeck_provider_v2* provider_table(Definition (*define)());

}

#define INFERDECK_PROVIDER_SDK(define_function)                                                       \
    extern "C" INFERDECK_PROVIDER_EXPORT const inferdeck_provider_v2* inferdeck_provider_v2(void)     \
    {                                                                                                 \
        return ::inferdeck::provider::sdk::provider_table(define_function);                           \
    }
