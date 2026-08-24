#pragma once

#include "foundation/result.hpp"
#include "model/imodel.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <optional>

namespace inferdeck::gateway {

struct ParsedResponsesRequest {
    std::string requested_model;
    model::InferenceRequest generation;
    bool stream{false};
    int priority{0};
    std::optional<std::string> capability_field;
    std::optional<std::string> capability;
};

foundation::Result<ParsedResponsesRequest> parse_openai_responses_request(
    const nlohmann::json& body, bool allow_extensions);

}
