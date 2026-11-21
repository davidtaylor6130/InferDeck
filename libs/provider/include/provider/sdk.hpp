#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "foundation/result.hpp"
#include "inferdeck/provider_abi.h"
#include "model/ibackend.hpp"

namespace inferdeck::provider::sdk {

struct Definition
{
    std::string manifest_json;
    std::function<std::unique_ptr<model::IBackend>(const model::ModelInfo& info, const nlohmann::json& gateway)> factory;
    std::function<foundation::Result<void>(const nlohmann::json& host)> init{};
    std::function<void()> shutdown{};
    std::function<foundation::Result<nlohmann::json>(const std::string& operation, const nlohmann::json& request)>
        operation{};
};

std::string contract_manifest(const std::string& runtime, const std::string& version);

const inferdeck_provider_v2* provider_table(Definition (*define)());

}

#define INFERDECK_PROVIDER_SDK(define_function)                                                       \
    extern "C" INFERDECK_PROVIDER_EXPORT const inferdeck_provider_v2* inferdeck_provider_v2(void)     \
    {                                                                                                 \
        return ::inferdeck::provider::sdk::provider_table(define_function);                           \
    }
