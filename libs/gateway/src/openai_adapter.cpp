#include "gateway/openai_adapter.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace inferdeck::gateway {

namespace {

foundation::Result<model::InferenceRequest> invalid(
    std::string message, std::string field = {}) {
    return foundation::Err<model::InferenceRequest>(
        foundation::ErrorCode::InvalidArgument, std::move(message),
        std::move(field));
}

foundation::Result<void> require_fields(
    const nlohmann::json& value,
    const std::unordered_set<std::string>& supported,
    const std::string& context) {
    for (const auto& field : value.items()) {
        if (!supported.contains(field.key())) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "unsupported " + context + " parameter: " + field.key(),
                field.key());
        }
    }
    return foundation::Ok();
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
    const nlohmann::json& value, bool allow_extensions,
    bool responses_content_types) {
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
    static const std::unordered_set<std::string> strict_fields{
        "role", "content", "name", "tool_call_id", "tool_calls",
    };
    if (!allow_extensions) {
        auto fields = require_fields(value, strict_fields, "message");
        if (!fields) {
            return foundation::Err<inference::Message>(
                fields.error().code, fields.error().message);
        }
    }
    inference::Message message;
    message.role = inference::Message::parse_role(role);
    if (value.contains("name") && !value["name"].is_string()) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "message name must be a string");
    }
    if (value.contains("name")) {
        message.name = value["name"].get<std::string>();
    }
    if (value.contains("tool_call_id") &&
        !value["tool_call_id"].is_string()) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "message tool_call_id must be a string");
    }
    if (value.contains("tool_call_id")) {
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
            const bool text_type = allow_extensions
                ? (type == "text" || type == "input_text")
                : (responses_content_types ? type == "input_text"
                                           : type == "text");
            const bool image_type = allow_extensions
                ? (type == "image" || type == "image_url" ||
                   type == "input_image")
                : (responses_content_types ? type == "input_image"
                                           : type == "image_url");
            if (text_type) {
                if (!allow_extensions) {
                    static const std::unordered_set<std::string> fields{
                        "type", "text",
                    };
                    auto allowed = require_fields(
                        part, fields, "message text content");
                    if (!allowed) {
                        return foundation::Err<inference::Message>(
                            allowed.error().code, allowed.error().message);
                    }
                }
                if (!part.contains("text") || !part["text"].is_string()) {
                    return foundation::Err<inference::Message>(
                        foundation::ErrorCode::InvalidArgument,
                        "text content requires string text");
                }
                message.content.emplace_back(inference::TextContent{
                    part["text"].get<std::string>()});
            } else if (image_type) {
                if (!allow_extensions) {
                    const std::unordered_set<std::string> fields =
                        responses_content_types
                        ? std::unordered_set<std::string>{
                              "type", "image_url", "detail"}
                        : std::unordered_set<std::string>{
                              "type", "image_url"};
                    auto allowed = require_fields(
                        part, fields, "message image content");
                    if (!allowed) {
                        return foundation::Err<inference::Message>(
                            allowed.error().code, allowed.error().message);
                    }
                }
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
    if (!value.contains("content") &&
        !(role == "assistant" && value.contains("tool_calls"))) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "message content is required");
    }
    if (role == "tool" && message.tool_call_id.empty()) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "tool messages require a non-empty tool_call_id");
    }
    if (value.contains("tool_calls")) {
        if (!value["tool_calls"].is_array()) {
            return foundation::Err<inference::Message>(
                foundation::ErrorCode::InvalidArgument,
                "message tool_calls must be an array");
        }
        for (const auto& item : value["tool_calls"]) {
            static const std::unordered_set<std::string> item_fields{
                "id", "type", "function",
            };
            if (!item.is_object() || !item.contains("type") ||
                !item["type"].is_string() ||
                item["type"].get<std::string>() != "function" ||
                !item.contains("function") ||
                !item["function"].is_object()) {
                return foundation::Err<inference::Message>(
                    foundation::ErrorCode::InvalidArgument,
                    "message tool_calls entries require type function");
            }
            if (!allow_extensions) {
                auto allowed = require_fields(
                    item, item_fields, "message tool call");
                if (!allowed) {
                    return foundation::Err<inference::Message>(
                        allowed.error().code, allowed.error().message);
                }
            }
            const auto& function = item["function"];
            static const std::unordered_set<std::string> function_fields{
                "name", "arguments",
            };
            if (!allow_extensions) {
                auto allowed = require_fields(
                    function, function_fields, "message tool call function");
                if (!allowed) {
                    return foundation::Err<inference::Message>(
                        allowed.error().code, allowed.error().message);
                }
            }
            if (!item.contains("id") || !item["id"].is_string() ||
                item["id"].get_ref<const std::string&>().empty() ||
                !function.contains("name") || !function["name"].is_string() ||
                function["name"].get_ref<const std::string&>().empty() ||
                !function.contains("arguments") ||
                (!allow_extensions && !function["arguments"].is_string())) {
                return foundation::Err<inference::Message>(
                    foundation::ErrorCode::InvalidArgument,
                    "message tool call requires non-empty string id and name and string arguments");
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
    return parse_message(value, allow_extensions, true);
}

foundation::Result<model::InferenceRequest> parse_openai_chat_request(
    const nlohmann::json& body, bool allow_extensions) {
    if (!body.is_object()) return invalid("request body must be an object");
    if (!body.contains("model") || !body["model"].is_string() ||
        body["model"].get_ref<const std::string&>().empty()) {
        return invalid("model must be a non-empty string", "model");
    }
    if (!body.contains("messages") || !body["messages"].is_array()) {
        return invalid("messages must be an array", "messages");
    }
    if (body["messages"].empty()) {
        return invalid("messages must not be empty", "messages");
    }
    if (body.contains("max_tokens") &&
        body.contains("max_completion_tokens")) {
        return invalid(
            "max_tokens and max_completion_tokens are mutually exclusive",
            "max_tokens");
    }
    const auto positive_integer = [](const nlohmann::json& value) {
        if (value.is_number_unsigned()) {
            return value.get<std::uint64_t>() > 0 &&
                   value.get<std::uint64_t>() <=
                       static_cast<std::uint64_t>(
                           std::numeric_limits<int>::max());
        }
        return value.is_number_integer() &&
               value.get<std::int64_t>() > 0 &&
               value.get<std::int64_t>() <=
                   std::numeric_limits<int>::max();
    };
    for (const auto field : {"max_tokens", "max_completion_tokens"}) {
        if (body.contains(field) && !body[field].is_null() &&
            !positive_integer(body[field])) {
            return invalid(std::string(field) +
                           " must be a positive integer", field);
        }
    }
    if (body.contains("temperature") && !body["temperature"].is_null()) {
        if (!body["temperature"].is_number()) {
            return invalid("temperature must be a number", "temperature");
        }
        const double value = body["temperature"].get<double>();
        if (!std::isfinite(value) || value < 0.0 || value > 2.0) {
            return invalid("temperature must be between 0 and 2", "temperature");
        }
    }
    if (body.contains("top_p") && !body["top_p"].is_null()) {
        if (!body["top_p"].is_number()) {
            return invalid("top_p must be a number", "top_p");
        }
        const double value = body["top_p"].get<double>();
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return invalid("top_p must be between 0 and 1", "top_p");
        }
    }
    if (body.contains("seed") && !body["seed"].is_null() &&
        !(body["seed"].is_number_integer() ||
          body["seed"].is_number_unsigned())) {
        return invalid("seed must be an integer", "seed");
    }
    if (body.contains("parallel_tool_calls") &&
        !body["parallel_tool_calls"].is_boolean()) {
        return invalid("parallel_tool_calls must be a boolean",
                       "parallel_tool_calls");
    }
    if (body.contains("reasoning_effort") &&
        !body["reasoning_effort"].is_null() &&
        !body["reasoning_effort"].is_string()) {
        return invalid("reasoning_effort must be a string",
                       "reasoning_effort");
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
        auto message = parse_message(value, allow_extensions, false);
        if (!message) return invalid(message.error().message, "messages");
        request.messages.push_back(std::move(*message));
    }
    if (body.contains("tools")) {
        if (!body["tools"].is_array()) {
            return invalid("tools must be an array", "tools");
        }
        for (const auto& item : body["tools"]) {
            static const std::unordered_set<std::string> tool_fields{
                "type", "function",
            };
            if (!item.is_object() || item.value("type", "") != "function" ||
                !item.contains("function") || !item["function"].is_object()) {
                return invalid("tools entries must be function tools", "tools");
            }
            if (!allow_extensions) {
                auto fields = require_fields(item, tool_fields, "tool");
                if (!fields) return invalid(fields.error().message, "tools");
            }
            const auto& function = item["function"];
            static const std::unordered_set<std::string> function_fields{
                "name", "description", "parameters", "strict",
            };
            if (!allow_extensions) {
                auto fields = require_fields(
                    function, function_fields, "function tool");
                if (!fields) return invalid(fields.error().message, "tools");
            }
            if (!function.contains("name") || !function["name"].is_string() ||
                function["name"].get_ref<const std::string&>().empty()) {
                return invalid("function tools require a string name", "tools");
            }
            if (function.contains("description") &&
                !function["description"].is_string()) {
                return invalid("function tool description must be a string",
                               "tools");
            }
            if (function.contains("parameters") &&
                !function["parameters"].is_object()) {
                return invalid("function tool parameters must be an object",
                               "tools");
            }
            if (function.contains("strict") &&
                !function["strict"].is_boolean()) {
                return invalid("function tool strict must be a boolean",
                               "tools");
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
            else return invalid("tool_choice string is unsupported",
                                "tool_choice");
        } else if (choice.is_object() && choice.value("type", "") == "function" &&
                   choice.contains("function") && choice["function"].is_object() &&
                   choice["function"].contains("name") &&
                   choice["function"]["name"].is_string()) {
            static const std::unordered_set<std::string> choice_fields{
                "type", "function",
            };
            static const std::unordered_set<std::string> function_fields{
                "name",
            };
            if (!allow_extensions) {
                auto fields = require_fields(
                    choice, choice_fields, "tool_choice");
                if (!fields) {
                    return invalid(fields.error().message, "tool_choice");
                }
                fields = require_fields(
                    choice["function"], function_fields,
                    "tool_choice.function");
                if (!fields) {
                    return invalid(fields.error().message, "tool_choice");
                }
            }
            if (choice["function"]["name"]
                    .get_ref<const std::string&>().empty()) {
                return invalid(
                    "tool_choice function name must not be empty",
                    "tool_choice");
            }
            request.tool_choice.kind = inference::ToolChoiceKind::Function;
            request.tool_choice.function_name =
                choice["function"]["name"].get<std::string>();
        } else {
            return invalid(
                "tool_choice must be auto, none, required, or a function",
                "tool_choice");
        }
    }
    if (body.contains("stop") && !body["stop"].is_null()) {
        if (body["stop"].is_string()) {
            const auto stop = body["stop"].get<std::string>();
            if (stop.empty()) return invalid("stop must not be empty", "stop");
            request.stop.push_back(stop);
        } else if (body["stop"].is_array()) {
            if (body["stop"].empty() || body["stop"].size() > 4) {
                return invalid("stop must contain 1 to 4 strings", "stop");
            }
            for (const auto& stop : body["stop"]) {
                if (!stop.is_string() ||
                    stop.get_ref<const std::string&>().empty()) {
                    return invalid(
                        "stop entries must be non-empty strings", "stop");
                }
                request.stop.push_back(stop.get<std::string>());
            }
        } else {
            return invalid("stop must be a string or array of strings",
                           "stop");
        }
    }
    if (body.contains("response_format") && !body["response_format"].is_null()) {
        if (!body["response_format"].is_object()) {
            return invalid("response_format must be an object",
                           "response_format");
        }
        const auto& format = body["response_format"];
        static const std::unordered_set<std::string> format_fields{
            "type", "json_schema",
        };
        if (!allow_extensions) {
            auto fields = require_fields(
                format, format_fields, "response_format");
            if (!fields) {
                return invalid(fields.error().message, "response_format");
            }
        }
        if (!format.contains("type") || !format["type"].is_string()) {
            return invalid("response_format requires string type",
                           "response_format");
        }
        const std::string type = format.value("type", "text");
        if (type == "json_object") {
            if (format.size() != 1) {
                return invalid(
                    "json_object response_format accepts only type",
                    "response_format");
            }
            request.output.kind = inference::StructuredOutputKind::JsonObject;
            request.output.schema = "{}";
        } else if (type == "json_schema" && format.contains("json_schema") &&
                   format["json_schema"].is_object()) {
            const auto& schema = format["json_schema"];
            static const std::unordered_set<std::string> schema_fields{
                "name", "description", "schema", "strict",
            };
            auto fields = require_fields(
                schema, schema_fields, "response_format.json_schema");
            if (!fields) {
                return invalid(fields.error().message, "response_format");
            }
            if (!schema.contains("name") || !schema["name"].is_string() ||
                schema["name"].get_ref<const std::string&>().empty() ||
                !schema.contains("schema") ||
                !schema["schema"].is_object()) {
                return invalid(
                    "json_schema requires non-empty name and object schema",
                    "response_format");
            }
            if (schema.contains("description") &&
                !schema["description"].is_string()) {
                return invalid("json_schema description must be a string",
                               "response_format");
            }
            if (schema.contains("strict") &&
                !schema["strict"].is_boolean()) {
                return invalid("json_schema strict must be a boolean",
                               "response_format");
            }
            request.output.kind = inference::StructuredOutputKind::JsonSchema;
            request.output.name = schema.value("name", "");
            request.output.description = schema.value("description", "");
            request.output.strict = schema.value("strict", false);
            request.output.schema = schema["schema"].dump();
        } else if (type == "json_schema") {
            return invalid(
                "json_schema response_format requires json_schema object",
                "response_format");
        } else if (type == "text" && format.size() != 1) {
            return invalid("text response_format accepts only type",
                           "response_format");
        } else if (type != "text") {
            return invalid("response_format type is unsupported",
                           "response_format");
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
