#include "llama_cpp_wrapper/llama_chat_adapter.hpp"

#include "mtmd-helper.h"

#include <algorithm>
#include <cstddef>

namespace inferdeck::llama_wrapper {

namespace {

std::string template_role(inference::MessageRole role) {
    if (role == inference::MessageRole::Developer) return "developer";
    if (role == inference::MessageRole::System) return "system";
    if (role == inference::MessageRole::Assistant) return "assistant";
    if (role == inference::MessageRole::Tool) return "tool";
    return "user";
}

common_reasoning_format reasoning_format(inference::ReasoningFormat value) {
    if (value == inference::ReasoningFormat::None) {
        return COMMON_REASONING_FORMAT_NONE;
    }
    if (value == inference::ReasoningFormat::DeepSeek) {
        return COMMON_REASONING_FORMAT_DEEPSEEK;
    }
    if (value == inference::ReasoningFormat::DeepSeekLegacy) {
        return COMMON_REASONING_FORMAT_DEEPSEEK_LEGACY;
    }
    return COMMON_REASONING_FORMAT_AUTO;
}

common_reasoning_format reasoning_format(const std::string& value) {
    if (value == "none") return COMMON_REASONING_FORMAT_NONE;
    if (value == "deepseek") return COMMON_REASONING_FORMAT_DEEPSEEK;
    if (value == "deepseek_legacy" || value == "deepseek-legacy") {
        return COMMON_REASONING_FORMAT_DEEPSEEK_LEGACY;
    }
    return COMMON_REASONING_FORMAT_AUTO;
}

common_chat_tool_choice tool_choice(const inference::ToolChoice& value) {
    if (value.kind == inference::ToolChoiceKind::None) {
        return COMMON_CHAT_TOOL_CHOICE_NONE;
    }
    if (value.kind == inference::ToolChoiceKind::Required ||
        value.kind == inference::ToolChoiceKind::Function) {
        return COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    }
    return COMMON_CHAT_TOOL_CHOICE_AUTO;
}

std::string quote_json_string(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string output{"\""};
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            output.push_back('\\');
            output.push_back(static_cast<char>(character));
        } else if (character < 0x20) {
            output += "\\u00";
            output.push_back(hex[(character >> 4) & 0xf]);
            output.push_back(hex[character & 0xf]);
        } else {
            output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
    return output;
}

foundation::Result<common_chat_msg> adapt_message(
    const inference::Message& message,
    std::vector<std::vector<std::uint8_t>>& media) {
    common_chat_msg output;
    output.role = template_role(message.role);
    output.reasoning_content = message.reasoning;
    output.tool_call_id = message.tool_call_id;
    output.tool_name = message.name;
    for (const auto& call : message.tool_calls) {
        common_chat_tool_call tool_call;
        tool_call.id = call.id;
        tool_call.name = call.name;
        tool_call.arguments = call.arguments;
        output.tool_calls.push_back(std::move(tool_call));
    }
    for (const auto& part : message.content) {
        common_chat_msg_content_part content;
        if (const auto* text = std::get_if<inference::TextContent>(&part)) {
            content.type = "text";
            content.text = text->text;
        } else if (const auto* image =
                       std::get_if<inference::ImageContent>(&part)) {
            if (image->bytes.empty()) {
                return foundation::Err<common_chat_msg>(
                    foundation::ErrorCode::InvalidArgument,
                    "image content must not be empty");
            }
            std::vector<std::uint8_t> bytes;
            bytes.reserve(image->bytes.size());
            for (const auto byte : image->bytes) {
                bytes.push_back(std::to_integer<std::uint8_t>(byte));
            }
            media.push_back(std::move(bytes));
            content.type = "media_marker";
            content.text = mtmd_default_marker();
        } else {
            return foundation::Err<common_chat_msg>(
                foundation::ErrorCode::InvalidArgument,
                "llama text generation does not support audio content");
        }
        output.content_parts.push_back(std::move(content));
    }
    return foundation::Ok(std::move(output));
}

}

foundation::Result<std::string> apply_reasoning_effort(
    common_chat_templates_inputs& inputs,
    const std::optional<std::string>& requested_value,
    const model::ModelInfo& info) {
    std::optional<std::string> requested = requested_value;
    if (!requested && info.reasoning.supported &&
        !info.reasoning.default_effort.empty()) {
        requested = info.reasoning.default_effort;
    }
    if (!requested) return foundation::Ok(std::string{});
    if (!info.reasoning.supported) {
        return foundation::Err<std::string>(
            foundation::ErrorCode::InvalidArgument,
            "model does not support reasoning effort: " + info.name);
    }
    std::string resolved = *requested;
    if (const auto alias = info.reasoning.aliases.find(resolved);
        alias != info.reasoning.aliases.end()) {
        resolved = alias->second;
    }
    if (resolved == "none") {
        if (!info.reasoning.none_disables) {
            return foundation::Err<std::string>(
                foundation::ErrorCode::InvalidArgument,
                "reasoning effort 'none' is not supported by model: " +
                    info.name);
        }
        inputs.enable_thinking = false;
        inputs.chat_template_kwargs.erase("reasoning_effort");
    } else {
        if (std::find(info.reasoning.efforts.begin(), info.reasoning.efforts.end(),
                      resolved) == info.reasoning.efforts.end()) {
            return foundation::Err<std::string>(
                foundation::ErrorCode::InvalidArgument,
                "unsupported reasoning effort '" + *requested +
                    "' for model: " + info.name);
        }
        inputs.chat_template_kwargs["reasoning_effort"] =
            quote_json_string(resolved);
    }
    return foundation::Ok(std::move(resolved));
}

foundation::Result<LlamaChatAdapterResult> adapt_generation_request(
    const inference::GenerationRequest& request,
    const model::ModelInfo& info,
    const LlamaChatAdapterOptions& options) {
    LlamaChatAdapterResult result;
    auto& inputs = result.inputs;
    inputs.reasoning_format = reasoning_format(options.default_reasoning_format);
    if (request.reasoning_format != inference::ReasoningFormat::Automatic) {
        inputs.reasoning_format = reasoning_format(request.reasoning_format);
    }
    inputs.add_generation_prompt = request.add_generation_prompt;
    inputs.use_jinja = true;
    inputs.enable_thinking = options.supports_thinking;
    if (request.enable_reasoning) inputs.enable_thinking = *request.enable_reasoning;
    inputs.parallel_tool_calls = options.supports_parallel_tool_calls &&
        request.parallel_tool_calls;
    for (const auto& message : request.messages) {
        auto adapted = adapt_message(message, result.media);
        if (!adapted) {
            return foundation::Err<LlamaChatAdapterResult>(
                adapted.error().code, adapted.error().message);
        }
        inputs.messages.push_back(std::move(*adapted));
    }
    if (inputs.messages.empty()) {
        common_chat_msg message;
        message.role = "user";
        message.content = request.prompt;
        inputs.messages.push_back(std::move(message));
    }
    for (const auto& function : request.tools) {
        if (request.tool_choice.kind == inference::ToolChoiceKind::Function &&
            function.name != request.tool_choice.function_name) {
            continue;
        }
        common_chat_tool tool;
        tool.name = function.name;
        tool.description = function.description;
        tool.parameters = function.parameters_schema;
        inputs.tools.push_back(std::move(tool));
    }
    inputs.tool_choice = tool_choice(request.tool_choice);
    if (!inputs.tools.empty() &&
        inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE &&
        request.output.kind == inference::StructuredOutputKind::Grammar) {
        return foundation::Err<LlamaChatAdapterResult>(
            foundation::ErrorCode::InvalidArgument,
            "custom grammar constraints cannot be combined with tools");
    }
    auto effort = apply_reasoning_effort(inputs, request.reasoning_effort, info);
    if (!effort) {
        return foundation::Err<LlamaChatAdapterResult>(
            effort.error().code, effort.error().message);
    }
    if (request.output.kind == inference::StructuredOutputKind::Grammar) {
        inputs.grammar = request.output.schema;
    } else if (request.output.kind ==
                   inference::StructuredOutputKind::JsonObject ||
               request.output.kind ==
                   inference::StructuredOutputKind::JsonSchema) {
        inputs.json_schema = request.output.schema.empty()
            ? "{}" : request.output.schema;
    }
    return foundation::Ok(std::move(result));
}

}
