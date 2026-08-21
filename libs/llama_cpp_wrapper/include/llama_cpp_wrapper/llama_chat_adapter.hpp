#pragma once

#include "chat.h"
#include "foundation/result.hpp"
#include "inference/domain.hpp"
#include "model/model_info.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace inferdeck::llama_wrapper {

struct LlamaChatAdapterOptions {
    bool supports_thinking{false};
    bool supports_parallel_tool_calls{false};
    std::string default_reasoning_format;
};

struct LlamaChatAdapterResult {
    common_chat_templates_inputs inputs;
    std::vector<std::vector<std::uint8_t>> media;
};

foundation::Result<std::string> apply_reasoning_effort(
    common_chat_templates_inputs& inputs,
    const std::optional<std::string>& requested,
    const model::ModelInfo& info);

foundation::Result<LlamaChatAdapterResult> adapt_generation_request(
    const inference::GenerationRequest& request,
    const model::ModelInfo& info,
    const LlamaChatAdapterOptions& options);

}
