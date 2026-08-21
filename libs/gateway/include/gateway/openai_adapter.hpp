#pragma once

#include "foundation/result.hpp"
#include "model/imodel.hpp"

#include <nlohmann/json_fwd.hpp>

namespace inferdeck::gateway {

foundation::Result<model::InferenceRequest> parse_openai_chat_request(
    const nlohmann::json& body, bool allow_extensions);

}
