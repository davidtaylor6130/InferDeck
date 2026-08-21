#include "gateway/openai_adapter.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <limits>
#include <string_view>

namespace inferdeck::gateway {

namespace {

foundation::Result<model::InferenceRequest> invalid(std::string message) {
    return foundation::Err<model::InferenceRequest>(
        foundation::ErrorCode::InvalidArgument, std::move(message));
}

foundation::Result<std::vector<std::byte>> decode_base64(
    std::string_view encoded) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> values{};
    values.fill(-1);
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        values[static_cast<unsigned char>(alphabet[index])] =
            static_cast<int>(index);
    }
    if (encoded.empty() || encoded.size() % 4 != 0) {
        return foundation::Err<std::vector<std::byte>>(
            foundation::ErrorCode::InvalidArgument,
            "image_url contains invalid base64 length");
    }
    std::size_t padding = 0;
    if (encoded.ends_with("==")) padding = 2;
    else if (encoded.ends_with('=')) padding = 1;
    const std::size_t data_size = encoded.size() - padding;
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const unsigned char character =
            static_cast<unsigned char>(encoded[index]);
        if (index >= data_size) {
            if (character != '=') {
                return foundation::Err<std::vector<std::byte>>(
                    foundation::ErrorCode::InvalidArgument,
                    "image_url contains invalid base64 padding");
            }
        } else if (values[character] < 0) {
            return foundation::Err<std::vector<std::byte>>(
                foundation::ErrorCode::InvalidArgument,
                "image_url contains invalid base64 data");
        }
    }
    std::vector<std::byte> output;
    output.reserve(encoded.size() / 4 * 3 - padding);
    for (std::size_t index = 0; index < encoded.size(); index += 4) {
        const int first = values[static_cast<unsigned char>(encoded[index])];
        const int second = values[static_cast<unsigned char>(encoded[index + 1])];
        const int third = encoded[index + 2] == '=' ? 0
            : values[static_cast<unsigned char>(encoded[index + 2])];
        const int fourth = encoded[index + 3] == '=' ? 0
            : values[static_cast<unsigned char>(encoded[index + 3])];
        if (first < 0 || second < 0 || third < 0 || fourth < 0) {
            return foundation::Err<std::vector<std::byte>>(
                foundation::ErrorCode::InvalidArgument,
                "image_url contains invalid base64 data");
        }
        const std::uint32_t value =
            (static_cast<std::uint32_t>(first) << 18) |
            (static_cast<std::uint32_t>(second) << 12) |
            (static_cast<std::uint32_t>(third) << 6) |
            static_cast<std::uint32_t>(fourth);
        output.push_back(static_cast<std::byte>((value >> 16) & 0xff));
        if (encoded[index + 2] != '=') {
            output.push_back(static_cast<std::byte>((value >> 8) & 0xff));
        }
        if (encoded[index + 3] != '=') {
            output.push_back(static_cast<std::byte>(value & 0xff));
        }
    }
    if ((padding == 1 &&
         (values[static_cast<unsigned char>(encoded[data_size - 1])] & 0x3) != 0) ||
        (padding == 2 &&
         (values[static_cast<unsigned char>(encoded[data_size - 1])] & 0xf) != 0)) {
        return foundation::Err<std::vector<std::byte>>(
            foundation::ErrorCode::InvalidArgument,
            "image_url contains non-canonical base64 padding");
    }
    if (output.empty()) {
        return foundation::Err<std::vector<std::byte>>(
            foundation::ErrorCode::InvalidArgument,
            "image_url decoded to empty data");
    }
    return foundation::Ok(std::move(output));
}

foundation::Result<inference::Content> parse_image(
    const nlohmann::json& part) {
    if (!part.contains("image_url")) {
        return foundation::Err<inference::Content>(
            foundation::ErrorCode::InvalidArgument,
            "image content requires image_url");
    }
    const auto& value = part["image_url"];
    std::string url;
    std::string detail;
    if (value.is_string()) {
        url = value.get<std::string>();
        if (part.contains("detail") && part["detail"].is_string()) {
            detail = part["detail"].get<std::string>();
        }
    } else if (value.is_object() && value.contains("url") &&
               value["url"].is_string()) {
        url = value["url"].get<std::string>();
        if (value.contains("detail") && value["detail"].is_string()) {
            detail = value["detail"].get<std::string>();
        }
    } else {
        return foundation::Err<inference::Content>(
            foundation::ErrorCode::InvalidArgument,
            "image_url must be a string or object with string url");
    }
    const auto comma = url.find(',');
    if (comma == std::string::npos || !url.starts_with("data:image/") ||
        url.substr(0, comma).find(";base64") == std::string::npos) {
        return foundation::Err<inference::Content>(
            foundation::ErrorCode::InvalidArgument,
            "image_url must be a base64 data:image URL");
    }
    if (url.size() - comma - 1 > 32U * 1024U * 1024U) {
        return foundation::Err<inference::Content>(
            foundation::ErrorCode::InvalidArgument,
            "image payload exceeds 32 MiB encoded");
    }
    auto bytes = decode_base64(std::string_view(url).substr(comma + 1));
    if (!bytes) {
        return foundation::Err<inference::Content>(bytes.error().code,
                                                   bytes.error().message);
    }
    inference::ImageContent content;
    content.bytes = std::move(*bytes);
    content.media_type = url.substr(5, url.find(';') - 5);
    content.detail = std::move(detail);
    return foundation::Ok(inference::Content{std::move(content)});
}

foundation::Result<inference::Message> parse_message(
    const nlohmann::json& value, bool allow_extensions) {
    if (!value.is_object() || !value.contains("role") ||
        !value["role"].is_string()) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "messages entries require a string role");
    }
    const std::string role = value["role"].get<std::string>();
    if (role != "developer" && role != "system" && role != "user" &&
        role != "assistant" && role != "tool") {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "messages role is unsupported");
    }
    inference::Message message;
    message.role = inference::Message::parse_role(role);
    if (value.contains("name") && value["name"].is_string()) {
        message.name = value["name"].get<std::string>();
    }
    if (value.contains("tool_call_id") && value["tool_call_id"].is_string()) {
        message.tool_call_id = value["tool_call_id"].get<std::string>();
    }
    if (allow_extensions && value.contains("reasoning_content") &&
        value["reasoning_content"].is_string()) {
        message.reasoning = value["reasoning_content"].get<std::string>();
    }
    if (value.contains("content") && value["content"].is_string()) {
        message.content.emplace_back(inference::TextContent{
            value["content"].get<std::string>()});
    } else if (value.contains("content") && value["content"].is_array()) {
        for (const auto& part : value["content"]) {
            if (!part.is_object() || !part.contains("type") ||
                !part["type"].is_string()) {
                return foundation::Err<inference::Message>(
                    foundation::ErrorCode::InvalidArgument,
                    "message content parts require a string type");
            }
            const std::string type = part["type"].get<std::string>();
            if (type == "text" || type == "input_text") {
                if (!part.contains("text") || !part["text"].is_string()) {
                    return foundation::Err<inference::Message>(
                        foundation::ErrorCode::InvalidArgument,
                        "text content requires string text");
                }
                message.content.emplace_back(inference::TextContent{
                    part["text"].get<std::string>()});
            } else if (type == "image" || type == "image_url" ||
                       type == "input_image") {
                auto content = parse_image(part);
                if (!content) {
                    return foundation::Err<inference::Message>(
                        content.error().code, content.error().message);
                }
                message.content.push_back(std::move(*content));
            } else {
                return foundation::Err<inference::Message>(
                    foundation::ErrorCode::InvalidArgument,
                    "message content type is unsupported: " + type);
            }
        }
    } else if (value.contains("content") && !value["content"].is_null()) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "message content must be a string, array, or null");
    }
    if (value.contains("tool_calls")) {
        if (!value["tool_calls"].is_array()) {
            return foundation::Err<inference::Message>(
                foundation::ErrorCode::InvalidArgument,
                "message tool_calls must be an array");
        }
        for (const auto& item : value["tool_calls"]) {
            if (!item.is_object() || !item.contains("type") ||
                !item["type"].is_string() ||
                item["type"].get<std::string>() != "function" ||
                !item.contains("function") ||
                !item["function"].is_object()) {
                return foundation::Err<inference::Message>(
                    foundation::ErrorCode::InvalidArgument,
                    "message tool_calls entries require type function");
            }
            const auto& function = item["function"];
            if (!function.contains("name") || !function["name"].is_string() ||
                !function.contains("arguments")) {
                return foundation::Err<inference::Message>(
                    foundation::ErrorCode::InvalidArgument,
                    "message tool call requires name and arguments");
            }
            inference::FunctionCall call;
            if (item.contains("id") && item["id"].is_string()) {
                call.id = item["id"].get<std::string>();
            }
            call.name = function["name"].get<std::string>();
            call.arguments = function["arguments"].is_string()
                ? function["arguments"].get<std::string>()
                : function["arguments"].dump();
            message.tool_calls.push_back(std::move(call));
        }
    }
    return foundation::Ok(std::move(message));
}

inference::ReasoningFormat reasoning_format(const std::string& value) {
    if (value == "none") return inference::ReasoningFormat::None;
    if (value == "deepseek") return inference::ReasoningFormat::DeepSeek;
    if (value == "deepseek_legacy" || value == "deepseek-legacy") {
        return inference::ReasoningFormat::DeepSeekLegacy;
    }
    return inference::ReasoningFormat::Automatic;
}

}

foundation::Result<inference::Message> parse_openai_message(
    const nlohmann::json& value, bool allow_extensions) {
    return parse_message(value, allow_extensions);
}

foundation::Result<model::InferenceRequest> parse_openai_chat_request(
    const nlohmann::json& body, bool allow_extensions) {
    if (!body.is_object()) return invalid("request body must be an object");
    if (!body.contains("messages") || !body["messages"].is_array()) {
        return invalid("messages must be an array");
    }
    model::InferenceRequest request;
    try {
        request.max_output_tokens = body.value(
            "max_tokens", body.value("max_completion_tokens",
                                      inference::kUseContextBudget));
        if (body.contains("temperature") && !body["temperature"].is_null()) {
            request.sampling.temperature = body["temperature"].get<float>();
        }
        if (body.contains("top_p") && !body["top_p"].is_null()) {
            request.sampling.top_p = body["top_p"].get<float>();
        }
        if (allow_extensions && body.contains("top_k") && !body["top_k"].is_null()) {
            request.sampling.top_k = body["top_k"].get<int>();
        }
        if (allow_extensions && body.contains("min_p") && !body["min_p"].is_null()) {
            request.sampling.min_p = body["min_p"].get<float>();
        }
        if (allow_extensions && body.contains("repeat_penalty") &&
            !body["repeat_penalty"].is_null()) {
            request.sampling.repeat_penalty = body["repeat_penalty"].get<float>();
        }
        if (allow_extensions && body.contains("repeat_last_n") &&
            !body["repeat_last_n"].is_null()) {
            request.sampling.repeat_last_n = body["repeat_last_n"].get<int>();
        }
        request.sampling.seed = body.value("seed", std::int64_t{-1});
        request.parallel_tool_calls = body.value("parallel_tool_calls", true);
        request.add_generation_prompt = allow_extensions
            ? body.value("add_generation_prompt", true) : true;
        if (allow_extensions && body.contains("reasoning_format") &&
            body["reasoning_format"].is_string()) {
            request.reasoning_format = reasoning_format(
                body["reasoning_format"].get<std::string>());
        }
        if (body.contains("reasoning_effort") &&
            body["reasoning_effort"].is_string()) {
            request.reasoning_effort =
                body["reasoning_effort"].get<std::string>();
        } else if (allow_extensions && body.contains("chat_template_kwargs") &&
                   body["chat_template_kwargs"].is_object() &&
                   body["chat_template_kwargs"].contains("reasoning_effort") &&
                   body["chat_template_kwargs"]["reasoning_effort"].is_string()) {
            request.reasoning_effort = body["chat_template_kwargs"]
                ["reasoning_effort"].get<std::string>();
        }
        if (allow_extensions && body.contains("chat_template_kwargs") &&
            body["chat_template_kwargs"].is_object() &&
            body["chat_template_kwargs"].contains("enable_thinking") &&
            body["chat_template_kwargs"]["enable_thinking"].is_boolean()) {
            request.enable_reasoning = body["chat_template_kwargs"]
                ["enable_thinking"].get<bool>();
        }
    } catch (const nlohmann::json::exception& error) {
        return invalid(error.what());
    }
    for (const auto& value : body["messages"]) {
        auto message = parse_message(value, allow_extensions);
        if (!message) return invalid(message.error().message);
        request.messages.push_back(std::move(*message));
    }
    if (body.contains("tools")) {
        if (!body["tools"].is_array()) return invalid("tools must be an array");
        for (const auto& item : body["tools"]) {
            if (!item.is_object() || item.value("type", "") != "function" ||
                !item.contains("function") || !item["function"].is_object()) {
                return invalid("tools entries must be function tools");
            }
            const auto& function = item["function"];
            if (!function.contains("name") || !function["name"].is_string()) {
                return invalid("function tools require a string name");
            }
            inference::FunctionTool tool;
            tool.name = function["name"].get<std::string>();
            if (function.contains("description") && function["description"].is_string()) {
                tool.description = function["description"].get<std::string>();
            }
            if (function.contains("parameters")) {
                tool.parameters_schema = function["parameters"].dump();
            }
            request.tools.push_back(std::move(tool));
        }
    }
    if (body.contains("tool_choice") && !body["tool_choice"].is_null()) {
        const auto& choice = body["tool_choice"];
        if (choice.is_string()) {
            const std::string value = choice.get<std::string>();
            if (value == "none") request.tool_choice.kind = inference::ToolChoiceKind::None;
            else if (value == "required") request.tool_choice.kind = inference::ToolChoiceKind::Required;
            else if (value == "auto") request.tool_choice.kind = inference::ToolChoiceKind::Auto;
            else return invalid("tool_choice string is unsupported");
        } else if (choice.is_object() && choice.value("type", "") == "function" &&
                   choice.contains("function") && choice["function"].is_object() &&
                   choice["function"].contains("name") &&
                   choice["function"]["name"].is_string()) {
            request.tool_choice.kind = inference::ToolChoiceKind::Function;
            request.tool_choice.function_name =
                choice["function"]["name"].get<std::string>();
        } else {
            return invalid("tool_choice must be auto, none, required, or a function");
        }
    }
    if (body.contains("stop") && !body["stop"].is_null()) {
        if (body["stop"].is_string()) {
            request.stop.push_back(body["stop"].get<std::string>());
        } else if (body["stop"].is_array()) {
            for (const auto& stop : body["stop"]) {
                if (!stop.is_string()) return invalid("stop entries must be strings");
                request.stop.push_back(stop.get<std::string>());
            }
        } else {
            return invalid("stop must be a string or array of strings");
        }
    }
    if (body.contains("response_format") && !body["response_format"].is_null()) {
        if (!body["response_format"].is_object()) {
            return invalid("response_format must be an object");
        }
        const auto& format = body["response_format"];
        const std::string type = format.value("type", "text");
        if (type == "json_object") {
            request.output.kind = inference::StructuredOutputKind::JsonObject;
            request.output.schema = format.contains("schema")
                ? format["schema"].dump() : "{}";
        } else if (type == "json_schema" && format.contains("json_schema") &&
                   format["json_schema"].is_object()) {
            const auto& schema = format["json_schema"];
            request.output.kind = inference::StructuredOutputKind::JsonSchema;
            request.output.name = schema.value("name", "");
            request.output.description = schema.value("description", "");
            request.output.strict = schema.value("strict", false);
            if (schema.contains("schema")) request.output.schema = schema["schema"].dump();
        } else if (type != "text") {
            return invalid("response_format type is unsupported");
        }
    }
    if (allow_extensions && body.contains("grammar") && body["grammar"].is_string()) {
        request.output.kind = inference::StructuredOutputKind::Grammar;
        request.output.schema = body["grammar"].get<std::string>();
    } else if (allow_extensions && body.contains("json_schema") &&
               !body["json_schema"].is_null()) {
        request.output.kind = inference::StructuredOutputKind::JsonSchema;
        request.output.schema = body["json_schema"].dump();
    }
    return foundation::Ok(std::move(request));
}

}
