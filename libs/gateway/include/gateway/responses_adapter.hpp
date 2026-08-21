#pragma once

#include "foundation/result.hpp"
#include "model/imodel.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace inferdeck::gateway {

struct ParsedResponsesRequest {
    std::string requested_model;
    model::InferenceRequest generation;
    bool stream{false};
    int priority{0};
};

foundation::Result<ParsedResponsesRequest> parse_openai_responses_request(
    const nlohmann::json& body, bool allow_extensions);

}
