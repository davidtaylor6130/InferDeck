#include "gateway/responses_adapter.hpp"

#include "gateway/openai_adapter.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace inferdeck::gateway {

namespace {

foundation::Result<ParsedResponsesRequest> invalid(std::string message) {
    return foundation::Err<ParsedResponsesRequest>(
        foundation::ErrorCode::InvalidArgument, std::move(message));
}

foundation::Result<void> require_fields(
    const nlohmann::json& value,
    const std::unordered_set<std::string>& supported,
    const std::string& context) {
    for (const auto& field : value.items()) {
        if (!supported.contains(field.key())) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "unsupported " + context + " parameter: " + field.key());
        }
    }
    return foundation::Ok();
}

bool integer_in_range(const nlohmann::json& value, std::int64_t minimum,
                      std::int64_t maximum) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        return number <= static_cast<std::uint64_t>(maximum) &&
               (minimum <= 0 ||
                number >= static_cast<std::uint64_t>(minimum));
    }
    if (!value.is_number_integer()) return false;
    const auto number = value.get<std::int64_t>();
    return number >= minimum && number <= maximum;
}

foundation::Result<void> append_message(
    model::InferenceRequest& request, const nlohmann::json& value) {
    auto message = parse_openai_message(value);
    if (!message) {
        return foundation::Err<void>(
            message.error().code, message.error().message);
    }
    request.messages.push_back(std::move(*message));
    return foundation::Ok();
}

foundation::Result<void> parse_input_item(
    model::InferenceRequest& request, const nlohmann::json& item) {
    if (!item.is_object()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "each input item must be an object");
    }
    if (item.contains("type") && !item["type"].is_string()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "input item type must be a string");
    }
    const std::string type = item.value("type", "message");
    if (type == "message") {
        static const std::unordered_set<std::string> fields{
            "type", "role", "content", "id", "status",
        };
        auto allowed = require_fields(item, fields, "message");
        if (!allowed) return allowed;
        if (!item.contains("role") || !item["role"].is_string() ||
            !item.contains("content")) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "message requires string role and content");
        }
        const auto role = item["role"].get<std::string>();
        if (role != "developer" && role != "system" && role != "user" &&
            role != "assistant") {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "unsupported message role: " + role);
        }
        if (item.contains("id") && !item["id"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "message id must be a string");
        }
        if (item.contains("status") && !item["status"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "message status must be a string");
        }
        return append_message(request, item);
    }
    if (type == "function_call") {
        static const std::unordered_set<std::string> fields{
            "type", "call_id", "id", "name", "arguments", "status",
        };
        auto allowed = require_fields(item, fields, "function_call");
        if (!allowed) return allowed;
        if (!item.contains("name") || !item["name"].is_string() ||
            item["name"].get_ref<const std::string&>().empty() ||
            !item.contains("arguments") ||
            !item["arguments"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "function_call requires string name and arguments");
        }
        std::string call_id;
        if (item.contains("call_id") && item["call_id"].is_string()) {
            call_id = item["call_id"].get<std::string>();
        } else if (item.contains("id") && item["id"].is_string()) {
            call_id = item["id"].get<std::string>();
        }
        if (call_id.empty()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "function_call requires non-empty call_id");
        }
        inference::Message message;
        message.role = inference::MessageRole::Assistant;
        message.content.emplace_back(inference::TextContent{});
        message.tool_calls.push_back(inference::FunctionCall{
            std::move(call_id),
            item["name"].get<std::string>(),
            item["arguments"].get<std::string>(),
        });
        request.messages.push_back(std::move(message));
        return foundation::Ok();
    }
    if (type == "function_call_output") {
        static const std::unordered_set<std::string> fields{
            "type", "call_id", "output", "id", "status",
        };
        auto allowed = require_fields(item, fields, "function_call_output");
        if (!allowed) return allowed;
        if (!item.contains("call_id") || !item["call_id"].is_string() ||
            item["call_id"].get_ref<const std::string&>().empty() ||
            !item.contains("output") ||
            !(item["output"].is_string() || item["output"].is_array())) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "function_call_output requires call_id and string or content-array output");
        }
        return append_message(request, nlohmann::json{
            {"role", "tool"},
            {"tool_call_id", item["call_id"]},
            {"content", item["output"]},
        });
    }
    return foundation::Err<void>(
        foundation::ErrorCode::InvalidArgument,
        "unsupported input item type: " + type);
}

foundation::Result<void> parse_tools(
    model::InferenceRequest& request, const nlohmann::json& tools) {
    if (!tools.is_array()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "tools must be an array");
    }
    for (const auto& tool : tools) {
        static const std::unordered_set<std::string> fields{
            "type", "name", "description", "parameters", "strict",
        };
        if (!tool.is_object()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "tools entries must be objects");
        }
        auto allowed = require_fields(tool, fields, "function tool");
        if (!allowed) return allowed;
        if (!tool.contains("type") || !tool["type"].is_string() ||
            tool["type"].get<std::string>() != "function") {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "only function tools are supported");
        }
        if (!tool.contains("name") || !tool["name"].is_string() ||
            tool["name"].get_ref<const std::string&>().empty()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "function tool requires string name");
        }
        if (tool.contains("description") &&
            !tool["description"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "function tool description must be a string");
        }
        if (tool.contains("parameters") &&
            !tool["parameters"].is_object()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "function tool parameters must be an object");
        }
        if (tool.contains("strict") && !tool["strict"].is_boolean()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "function tool strict must be a boolean");
        }
        inference::FunctionTool parsed;
        parsed.name = tool["name"].get<std::string>();
        parsed.description = tool.value("description", std::string{});
        parsed.parameters_schema =
            tool.value("parameters", nlohmann::json::object()).dump();
        request.tools.push_back(std::move(parsed));
    }
    return foundation::Ok();
}

foundation::Result<void> parse_tool_choice(
    model::InferenceRequest& request, const nlohmann::json& choice) {
    if (choice.is_string()) {
        const auto value = choice.get<std::string>();
        if (value == "auto") {
            request.tool_choice.kind = inference::ToolChoiceKind::Auto;
        } else if (value == "none") {
            request.tool_choice.kind = inference::ToolChoiceKind::None;
        } else if (value == "required") {
            request.tool_choice.kind = inference::ToolChoiceKind::Required;
        } else {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "tool_choice must be auto, none, or required");
        }
        return foundation::Ok();
    }
    static const std::unordered_set<std::string> fields{"type", "name"};
    if (!choice.is_object()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "unsupported tool_choice");
    }
    auto allowed = require_fields(choice, fields, "tool_choice");
    if (!allowed) return allowed;
    if (!choice.contains("type") || !choice["type"].is_string() ||
        choice["type"].get<std::string>() != "function" ||
        !choice.contains("name") || !choice["name"].is_string() ||
        choice["name"].get_ref<const std::string&>().empty()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "function tool_choice requires name");
    }
    request.tool_choice.kind = inference::ToolChoiceKind::Function;
    request.tool_choice.function_name = choice["name"].get<std::string>();
    return foundation::Ok();
}

foundation::Result<void> parse_text(
    model::InferenceRequest& request, const nlohmann::json& text) {
    static const std::unordered_set<std::string> fields{"format"};
    if (!text.is_object()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "text must be an object");
    }
    auto allowed = require_fields(text, fields, "text");
    if (!allowed) return allowed;
    if (!text.contains("format")) return foundation::Ok();
    const auto& format = text["format"];
    if (!format.is_object() ||
        (format.contains("type") && !format["type"].is_string())) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "text.format must be an object with string type");
    }
    const auto type = format.value("type", "text");
    if (type == "text") {
        if (format.size() != 1) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "text format accepts only type");
        }
        return foundation::Ok();
    }
    if (type == "json_object") {
        if (format.size() != 1) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "json_object format accepts only type");
        }
        request.output.kind =
            inference::StructuredOutputKind::JsonObject;
        request.output.schema = "{}";
        return foundation::Ok();
    }
    if (type != "json_schema") {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "unsupported text format: " + type);
    }
    static const std::unordered_set<std::string> schema_fields{
        "type", "name", "description", "schema", "strict",
    };
    auto schema_allowed =
        require_fields(format, schema_fields, "text.format");
    if (!schema_allowed) return schema_allowed;
    if (!format.contains("name") || !format["name"].is_string() ||
        format["name"].get_ref<const std::string&>().empty() ||
        !format.contains("schema") || !format["schema"].is_object()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "json_schema format requires string name and object schema");
    }
    if (format.contains("description") &&
        !format["description"].is_string()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "json_schema description must be a string");
    }
    if (format.contains("strict") && !format["strict"].is_boolean()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "json_schema strict must be a boolean");
    }
    request.output.kind = inference::StructuredOutputKind::JsonSchema;
    request.output.name = format["name"].get<std::string>();
    request.output.description =
        format.value("description", std::string{});
    request.output.schema = format["schema"].dump();
    request.output.strict = format.value("strict", false);
    return foundation::Ok();
}

foundation::Result<void> validate_stateless_fields(
    const nlohmann::json& body) {
    for (const auto field : {
             "conversation", "previous_response_id", "prompt_cache_key",
             "prompt_cache_options", "prompt_cache_retention"}) {
        if (body.contains(field) && !body[field].is_null()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                std::string(field) +
                    " is unsupported by the stateless Responses implementation");
        }
    }
    if (body.contains("store") && !body["store"].is_null() &&
        (!body["store"].is_boolean() || body["store"].get<bool>())) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "unsupported Responses parameter: store");
    }
    if (body.contains("background") && !body["background"].is_null() &&
        (!body["background"].is_boolean() ||
         body["background"].get<bool>())) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "unsupported Responses parameter: background");
    }
    if (body.contains("include") &&
        (!body["include"].is_array() || !body["include"].empty())) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "include must be an empty array because expansions are unsupported");
    }
    if (body.contains("truncation") && !body["truncation"].is_null() &&
        (!body["truncation"].is_string() ||
         body["truncation"].get<std::string>() != "disabled")) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "truncation must be disabled");
    }
    if (body.contains("service_tier") &&
        !body["service_tier"].is_null()) {
        if (!body["service_tier"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "service_tier must be a string");
        }
        const auto tier = body["service_tier"].get<std::string>();
        if (tier != "auto" && tier != "default") {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "service_tier must be auto or default");
        }
    }
    return foundation::Ok();
}

}

foundation::Result<ParsedResponsesRequest> parse_openai_responses_request(
    const nlohmann::json& body, bool allow_extensions) {
    static const std::unordered_set<std::string> fields{
        "model", "input", "instructions", "max_output_tokens",
        "temperature", "top_p", "tools", "tool_choice", "text", "stream",
        "metadata", "parallel_tool_calls", "reasoning", "store",
        "background", "conversation", "previous_response_id", "include",
        "truncation", "service_tier", "prompt_cache_key",
        "prompt_cache_options", "prompt_cache_retention", "priority",
    };
    if (!body.is_object()) return invalid("request body must be an object");
    auto allowed = require_fields(body, fields, "Responses");
    if (!allowed) return invalid(allowed.error().message);
    if (!body.contains("model") || !body["model"].is_string() ||
        body["model"].get_ref<const std::string&>().empty()) {
        return invalid(
            "request body must include non-empty string 'model'");
    }
    if (!body.contains("input")) {
        return invalid("request body must include 'input'");
    }
    if (!allow_extensions && body.contains("priority")) {
        return invalid("unsupported Responses parameter: priority");
    }
    auto stateless = validate_stateless_fields(body);
    if (!stateless) return invalid(stateless.error().message);

    ParsedResponsesRequest parsed;
    parsed.requested_model = body["model"].get<std::string>();
    auto& request = parsed.generation;
    if (body.contains("stream") && !body["stream"].is_boolean()) {
        return invalid("stream must be a boolean");
    }
    parsed.stream = body.value("stream", false);
    if (body.contains("priority")) {
        if (!integer_in_range(body["priority"], -100, 100)) {
            return invalid(
                "priority must be an integer between -100 and 100");
        }
        parsed.priority = body["priority"].get<int>();
    }
    if (body.contains("max_output_tokens")) {
        if (!integer_in_range(body["max_output_tokens"], 1,
                              std::numeric_limits<int>::max())) {
            return invalid(
                "max_output_tokens must be a positive integer");
        }
        request.max_output_tokens =
            body["max_output_tokens"].get<int>();
    }
    for (const auto field : {"temperature", "top_p"}) {
        if (!body.contains(field)) continue;
        if (!body[field].is_number()) {
            return invalid(std::string(field) + " must be a number");
        }
        const double value = body[field].get<double>();
        const double maximum =
            std::string_view(field) == "temperature" ? 2.0 : 1.0;
        if (!std::isfinite(value) || value < 0.0 || value > maximum) {
            return invalid(std::string(field) + " is out of range");
        }
        if (std::string_view(field) == "temperature") {
            request.sampling.temperature = static_cast<float>(value);
        } else {
            request.sampling.top_p = static_cast<float>(value);
        }
    }
    if (body.contains("parallel_tool_calls")) {
        if (!body["parallel_tool_calls"].is_boolean()) {
            return invalid("parallel_tool_calls must be a boolean");
        }
        request.parallel_tool_calls =
            body["parallel_tool_calls"].get<bool>();
    }
    if (body.contains("metadata")) {
        if (!body["metadata"].is_object() ||
            body["metadata"].size() > 16) {
            return invalid(
                "metadata must be an object with at most 16 entries");
        }
        for (const auto& [key, value] : body["metadata"].items()) {
            if (key.size() > 64 || !value.is_string() ||
                value.get_ref<const std::string&>().size() > 512) {
                return invalid(
                    "metadata keys and string values exceed their limits");
            }
        }
    }
    if (body.contains("reasoning")) {
        static const std::unordered_set<std::string> reasoning_fields{
            "effort", "summary",
        };
        if (!body["reasoning"].is_object()) {
            return invalid("reasoning must be an object");
        }
        auto reasoning_allowed = require_fields(
            body["reasoning"], reasoning_fields, "reasoning");
        if (!reasoning_allowed) {
            return invalid(reasoning_allowed.error().message);
        }
        if (body["reasoning"].contains("summary") &&
            !body["reasoning"]["summary"].is_null()) {
            return invalid("reasoning.summary is unsupported");
        }
        if (body["reasoning"].contains("effort") &&
            !body["reasoning"]["effort"].is_null()) {
            if (!body["reasoning"]["effort"].is_string()) {
                return invalid("reasoning.effort must be a string");
            }
            request.reasoning_effort =
                body["reasoning"]["effort"].get<std::string>();
        }
    }
    if (body.contains("instructions")) {
        if (!body["instructions"].is_string()) {
            return invalid("instructions must be a string");
        }
        request.messages.emplace_back(
            inference::MessageRole::Developer,
            body["instructions"].get<std::string>());
    }
    const auto& input = body["input"];
    if (input.is_string()) {
        request.messages.emplace_back(
            inference::MessageRole::User, input.get<std::string>());
    } else if (input.is_array() && !input.empty()) {
        for (const auto& item : input) {
            auto result = parse_input_item(request, item);
            if (!result) return invalid(result.error().message);
        }
    } else {
        return invalid("input must be a string or non-empty array");
    }
    if (body.contains("tools")) {
        auto result = parse_tools(request, body["tools"]);
        if (!result) return invalid(result.error().message);
    }
    if (body.contains("tool_choice")) {
        auto result = parse_tool_choice(request, body["tool_choice"]);
        if (!result) return invalid(result.error().message);
    }
    if (body.contains("text")) {
        auto result = parse_text(request, body["text"]);
        if (!result) return invalid(result.error().message);
    }
    return foundation::Ok(std::move(parsed));
}

}
