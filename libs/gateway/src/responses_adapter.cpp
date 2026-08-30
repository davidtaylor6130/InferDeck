#include "gateway/responses_adapter.hpp"

#include "gateway/openai_adapter.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace inferdeck::gateway {

namespace {

foundation::Result<ParsedResponsesRequest> invalid(
    std::string message, std::string field = {}) {
    return foundation::Err<ParsedResponsesRequest>(
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

#include "responses_input_parser.ipp"

foundation::Result<void> parse_tools(
    model::InferenceRequest& request, const nlohmann::json& tools,
    std::optional<std::string>& unsupported_type) {
    if (!tools.is_array()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "tools must be an array");
    }
    for (const auto& tool : tools) {
        if (!tool.is_object()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "tools entries must be objects");
        }
        if (!tool.contains("type") || !tool["type"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "tool type must be a string");
        }
        const auto type = tool["type"].get<std::string>();
        if (type != "function") {
            static const std::unordered_set<std::string> official_types{
                "apply_patch", "code_interpreter", "computer",
                "computer_use", "computer_use_preview", "custom",
                "file_search", "image_generation", "local_shell", "mcp",
                "namespace", "programmatic_tool_calling", "shell",
                "tool_search", "web_search", "web_search_preview",
                "web_search_preview_2025_03_11",
            };
            if (!official_types.contains(type)) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "unsupported tool type: " + type);
            }
            if (!unsupported_type) unsupported_type = type;
            continue;
        }
        static const std::unordered_set<std::string> fields{
            "type", "name", "description", "parameters", "strict",
        };
        auto allowed = require_fields(tool, fields, "function tool");
        if (!allowed) return allowed;
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
    model::InferenceRequest& request, const nlohmann::json& choice,
    std::optional<std::string>& unsupported_type) {
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
    if (!choice.is_object()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "unsupported tool_choice");
    }
    if (!choice.contains("type") || !choice["type"].is_string()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "tool_choice type must be a string");
    }
    const auto type = choice["type"].get<std::string>();
    if (type != "function") {
        static const std::unordered_set<std::string> official_types{
            "allowed_tools", "apply_patch", "custom", "file_search",
            "web_search_preview", "computer", "computer_use_preview",
            "computer_use", "image_generation", "code_interpreter", "mcp",
            "programmatic_tool_calling", "shell", "tool_search",
        };
        if (!official_types.contains(type)) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "unsupported tool_choice type: " + type);
        }
        unsupported_type = type;
        return foundation::Ok();
    }
    static const std::unordered_set<std::string> fields{"type", "name"};
    auto allowed = require_fields(choice, fields, "tool_choice");
    if (!allowed) return allowed;
    if (!choice.contains("name") || !choice["name"].is_string() ||
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
    static const std::unordered_set<std::string> fields{"format", "verbosity"};
    if (!text.is_object()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "text must be an object");
    }
    auto allowed = require_fields(text, fields, "text");
    if (!allowed) return allowed;
    if (text.contains("verbosity") && !text["verbosity"].is_null() &&
        (!text["verbosity"].is_string() ||
         (text["verbosity"] != "low" && text["verbosity"] != "medium" &&
          text["verbosity"] != "high"))) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "text.verbosity must be low, medium, or high");
    }
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
             "previous_response_id", "prompt_cache_key",
             "safety_identifier", "user"}) {
        if (body.contains(field) && !body[field].is_null() &&
            !body[field].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                std::string(field) + " must be a string", field);
        }
    }
    if (body.contains("safety_identifier") &&
        body["safety_identifier"].is_string() &&
        body["safety_identifier"].get_ref<const std::string&>().size() > 64) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "safety_identifier must be at most 64 characters",
            "safety_identifier");
    }
    for (const auto field : {"store", "background"}) {
        if (body.contains(field) && !body[field].is_null() &&
            !body[field].is_boolean()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                std::string(field) + " must be a boolean", field);
        }
    }
    if (body.contains("conversation") && !body["conversation"].is_null() &&
        !(body["conversation"].is_string() ||
          body["conversation"].is_object())) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "conversation must be a string or object", "conversation");
    }
    if (body.contains("conversation") && !body["conversation"].is_null() &&
        body.contains("previous_response_id") &&
        !body["previous_response_id"].is_null()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "conversation and previous_response_id are mutually exclusive",
            "conversation");
    }
    if (body.contains("include") && !body["include"].is_null() &&
        !body["include"].is_array()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "include must be an array", "include");
    }
    if (body.contains("include") && body["include"].is_array()) {
        static const std::unordered_set<std::string> values{
            "file_search_call.results", "web_search_call.results",
            "web_search_call.action.sources", "message.input_image.image_url",
            "computer_call_output.output.image_url",
            "code_interpreter_call.outputs", "reasoning.encrypted_content",
            "message.output_text.logprobs",
        };
        for (const auto& value : body["include"]) {
            if (!value.is_string() ||
                !values.contains(value.get<std::string>())) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "include contains an invalid value", "include");
            }
        }
    }
    if (body.contains("truncation") && !body["truncation"].is_null() &&
        (!body["truncation"].is_string() ||
         (body["truncation"] != "auto" &&
          body["truncation"] != "disabled"))) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "truncation must be auto or disabled", "truncation");
    }
    if (body.contains("service_tier") &&
        !body["service_tier"].is_null()) {
        if (!body["service_tier"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "service_tier must be a string", "service_tier");
        }
        const auto tier = body["service_tier"].get<std::string>();
        static const std::unordered_set<std::string> tiers{
            "auto", "default", "flex", "scale", "priority", "fast",
            "ultrafast",
        };
        if (!tiers.contains(tier)) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "service_tier is invalid", "service_tier");
        }
    }
    if (body.contains("prompt_cache_retention") &&
        !body["prompt_cache_retention"].is_null() &&
        (!body["prompt_cache_retention"].is_string() ||
         (body["prompt_cache_retention"] != "in_memory" &&
          body["prompt_cache_retention"] != "24h"))) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "prompt_cache_retention must be in_memory or 24h",
            "prompt_cache_retention");
    }
    if (body.contains("prompt_cache_options") &&
        !body["prompt_cache_options"].is_null()) {
        if (!body["prompt_cache_options"].is_object()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "prompt_cache_options must be an object",
                "prompt_cache_options");
        }
        static const std::unordered_set<std::string> fields{"mode", "ttl"};
        auto allowed = require_fields(
            body["prompt_cache_options"], fields, "prompt_cache_options");
        if (!allowed) return allowed;
        const auto& options = body["prompt_cache_options"];
        if (options.contains("mode") &&
            (!options["mode"].is_string() ||
             (options["mode"] != "implicit" &&
              options["mode"] != "explicit"))) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "prompt_cache_options.mode is invalid",
                "prompt_cache_options");
        }
        if (options.contains("ttl") &&
            (!options["ttl"].is_string() || options["ttl"] != "30m")) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "prompt_cache_options.ttl must be 30m",
                "prompt_cache_options");
        }
    }
    if (body.contains("context_management") &&
        !body["context_management"].is_null()) {
        if (!body["context_management"].is_array()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "context_management must be an array", "context_management");
        }
        static const std::unordered_set<std::string> fields{
            "type", "compact_threshold",
        };
        for (const auto& entry : body["context_management"]) {
            if (!entry.is_object()) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "context_management entries must be objects",
                    "context_management");
            }
            auto allowed = require_fields(entry, fields, "context_management");
            if (!allowed) return allowed;
            if (!entry.contains("type") || !entry["type"].is_string() ||
                entry["type"].get_ref<const std::string&>().empty()) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "context_management.type must be a non-empty string",
                    "context_management");
            }
            if (entry.contains("compact_threshold") &&
                !entry["compact_threshold"].is_null() &&
                !integer_in_range(entry["compact_threshold"], 1,
                                  std::numeric_limits<int>::max())) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "context_management.compact_threshold must be positive",
                    "context_management");
            }
        }
    }
    if (body.contains("moderation") && !body["moderation"].is_null()) {
        const auto& moderation = body["moderation"];
        static const std::unordered_set<std::string> fields{"model", "policy"};
        if (!moderation.is_object()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "moderation must be an object", "moderation");
        }
        auto allowed = require_fields(moderation, fields, "moderation");
        if (!allowed) return allowed;
        if (!moderation.contains("model") || !moderation["model"].is_string() ||
            moderation["model"].get_ref<const std::string&>().empty()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "moderation.model must be a non-empty string", "moderation");
        }
        if (moderation.contains("policy") && !moderation["policy"].is_null()) {
            const auto& policy = moderation["policy"];
            static const std::unordered_set<std::string> policy_fields{
                "input", "output",
            };
            if (!policy.is_object()) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "moderation.policy must be an object", "moderation");
            }
            auto policy_allowed = require_fields(policy, policy_fields,
                                                 "moderation.policy");
            if (!policy_allowed) return policy_allowed;
            for (const auto field : {"input", "output"}) {
                if (!policy.contains(field) || policy[field].is_null()) continue;
                if (!policy[field].is_object() || policy[field].size() != 1 ||
                    !policy[field].contains("mode") ||
                    !policy[field]["mode"].is_string() ||
                    (policy[field]["mode"] != "score" &&
                     policy[field]["mode"] != "block")) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        std::string("moderation.policy.") + field +
                            ".mode must be score or block",
                        "moderation");
                }
            }
        }
    }
    if (body.contains("prompt") && !body["prompt"].is_null()) {
        const auto& prompt = body["prompt"];
        static const std::unordered_set<std::string> fields{
            "id", "variables", "version",
        };
        if (!prompt.is_object()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "prompt must be an object", "prompt");
        }
        auto allowed = require_fields(prompt, fields, "prompt");
        if (!allowed) return allowed;
        if (!prompt.contains("id") || !prompt["id"].is_string() ||
            prompt["id"].get_ref<const std::string&>().empty()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "prompt.id must be a non-empty string", "prompt");
        }
        if (prompt.contains("version") && !prompt["version"].is_null() &&
            !prompt["version"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "prompt.version must be a string", "prompt");
        }
        if (prompt.contains("variables") && !prompt["variables"].is_null() &&
            !prompt["variables"].is_object()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "prompt.variables must be an object", "prompt");
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
        "context_management", "moderation", "prompt", "safety_identifier",
        "stream_options", "top_logprobs", "user",
    };
    if (!body.is_object()) return invalid("request body must be an object");
    auto allowed = require_fields(body, fields, "Responses");
    if (!allowed) {
        return invalid(allowed.error().message, allowed.error().field);
    }
    if (!body.contains("model") || !body["model"].is_string() ||
        body["model"].get_ref<const std::string&>().empty()) {
        return invalid(
            "request body must include non-empty string 'model'", "model");
    }
    if (!body.contains("input") && !body.contains("prompt")) {
        return invalid("request body must include 'input' or 'prompt'",
                       "input");
    }
    if (!allow_extensions && body.contains("priority")) {
        return invalid("unsupported Responses parameter: priority",
                       "priority");
    }
    auto stateless = validate_stateless_fields(body);
    if (!stateless) {
        return invalid(stateless.error().message, stateless.error().field);
    }

    ParsedResponsesRequest parsed;
    parsed.requested_model = body["model"].get<std::string>();
    auto& request = parsed.generation;
    if (body.contains("stream") && !body["stream"].is_null() &&
        !body["stream"].is_boolean()) {
        return invalid("stream must be a boolean", "stream");
    }
    parsed.stream =
        body.contains("stream") && body["stream"].is_boolean()
            ? body["stream"].get<bool>() : false;
    if (body.contains("stream_options") &&
        !body["stream_options"].is_null()) {
        if (!body["stream_options"].is_object()) {
            return invalid("stream_options must be an object",
                           "stream_options");
        }
        static const std::unordered_set<std::string> stream_fields{
            "include_obfuscation",
        };
        auto stream_allowed = require_fields(
            body["stream_options"], stream_fields, "stream_options");
        if (!stream_allowed) {
            return invalid(stream_allowed.error().message,
                           "stream_options");
        }
        if (body["stream_options"].contains("include_obfuscation") &&
            !body["stream_options"]["include_obfuscation"].is_boolean()) {
            return invalid(
                "stream_options.include_obfuscation must be a boolean",
                "stream_options");
        }
        if (!parsed.stream) {
            return invalid("stream_options requires stream to be true",
                           "stream_options");
        }
    }
    if (body.contains("priority")) {
        if (!integer_in_range(body["priority"], -100, 100)) {
            return invalid(
                "priority must be an integer between -100 and 100",
                "priority");
        }
        parsed.priority = body["priority"].get<int>();
    }
    if (body.contains("max_output_tokens") &&
        !body["max_output_tokens"].is_null()) {
        if (!integer_in_range(body["max_output_tokens"], 1,
                              std::numeric_limits<int>::max())) {
            return invalid(
                "max_output_tokens must be a positive integer",
                "max_output_tokens");
        }
        request.max_output_tokens =
            body["max_output_tokens"].get<int>();
    }
    for (const auto field : {"temperature", "top_p"}) {
        if (!body.contains(field) || body[field].is_null()) continue;
        if (!body[field].is_number()) {
            return invalid(std::string(field) + " must be a number", field);
        }
        const double value = body[field].get<double>();
        const double maximum =
            std::string_view(field) == "temperature" ? 2.0 : 1.0;
        if (!std::isfinite(value) || value < 0.0 || value > maximum) {
            return invalid(std::string(field) + " is out of range", field);
        }
        if (std::string_view(field) == "temperature") {
            request.sampling.temperature = static_cast<float>(value);
        } else {
            request.sampling.top_p = static_cast<float>(value);
        }
    }
    if (body.contains("parallel_tool_calls") &&
        !body["parallel_tool_calls"].is_null()) {
        if (!body["parallel_tool_calls"].is_boolean()) {
            return invalid("parallel_tool_calls must be a boolean",
                           "parallel_tool_calls");
        }
        request.parallel_tool_calls =
            body["parallel_tool_calls"].get<bool>();
    }
    if (body.contains("metadata") && !body["metadata"].is_null()) {
        if (!body["metadata"].is_object() ||
            body["metadata"].size() > 16) {
            return invalid(
                "metadata must be an object with at most 16 entries",
                "metadata");
        }
        for (const auto& [key, value] : body["metadata"].items()) {
            if (key.size() > 64 || !value.is_string() ||
                value.get_ref<const std::string&>().size() > 512) {
                return invalid(
                    "metadata keys and string values exceed their limits",
                    "metadata");
            }
        }
    }
    if (body.contains("reasoning") && !body["reasoning"].is_null()) {
        static const std::unordered_set<std::string> reasoning_fields{
            "effort", "summary",
        };
        if (!body["reasoning"].is_object()) {
            return invalid("reasoning must be an object", "reasoning");
        }
        auto reasoning_allowed = require_fields(
            body["reasoning"], reasoning_fields, "reasoning");
        if (!reasoning_allowed) {
            return invalid(reasoning_allowed.error().message, "reasoning");
        }
        if (body["reasoning"].contains("summary") &&
            !body["reasoning"]["summary"].is_null()) {
            return invalid("reasoning.summary is unsupported", "reasoning");
        }
        if (body["reasoning"].contains("effort") &&
            !body["reasoning"]["effort"].is_null()) {
            if (!body["reasoning"]["effort"].is_string()) {
                return invalid("reasoning.effort must be a string",
                               "reasoning");
            }
            request.reasoning_effort =
                body["reasoning"]["effort"].get<std::string>();
        }
    }
    if (body.contains("instructions") && !body["instructions"].is_null()) {
        if (!body["instructions"].is_string()) {
            return invalid("instructions must be a string", "instructions");
        }
        request.messages.emplace_back(
            inference::MessageRole::Developer,
            body["instructions"].get<std::string>());
    }
    if (body.contains("input")) {
        const auto& input = body["input"];
        if (input.is_string()) {
            request.messages.emplace_back(
                inference::MessageRole::User, input.get<std::string>());
        } else if (input.is_array() && !input.empty()) {
            std::optional<std::string> unsupported_input_type;
            for (const auto& item : input) {
                auto result = parse_input_item(request, item,
                                               unsupported_input_type);
                if (!result) return invalid(result.error().message, "input");
            }
            if (unsupported_input_type) {
                parsed.capability_field = "input";
                parsed.capability =
                    "Responses input item type " + *unsupported_input_type;
            }
        } else {
            return invalid(
                "input must be a string or non-empty array", "input");
        }
    }
    if (body.contains("top_logprobs") &&
        !body["top_logprobs"].is_null()) {
        if (!integer_in_range(body["top_logprobs"], 0, 20)) {
            return invalid("top_logprobs must be between 0 and 20",
                           "top_logprobs");
        }
        request.logprobs = true;
        request.top_logprobs = body["top_logprobs"].get<int>();
    }
    if (body.contains("include") && body["include"].is_array() &&
        std::find(body["include"].begin(), body["include"].end(),
                  "message.output_text.logprobs") != body["include"].end()) {
        request.logprobs = true;
    }
    std::optional<std::string> unsupported_tool_type;
    if (body.contains("tools") && !body["tools"].is_null()) {
        auto result = parse_tools(request, body["tools"],
                                  unsupported_tool_type);
        if (!result) return invalid(result.error().message, "tools");
    }
    std::optional<std::string> unsupported_choice_type;
    if (body.contains("tool_choice") && !body["tool_choice"].is_null()) {
        auto result = parse_tool_choice(request, body["tool_choice"],
                                        unsupported_choice_type);
        if (!result) return invalid(result.error().message, "tool_choice");
    }
    if (body.contains("text") && !body["text"].is_null()) {
        auto result = parse_text(request, body["text"]);
        if (!result) return invalid(result.error().message, "text");
    }
    const auto require_capability = [&](std::string field,
                                        std::string capability) {
        if (!parsed.capability_field) {
            parsed.capability_field = std::move(field);
            parsed.capability = std::move(capability);
        }
    };
    if (body.contains("input")) {
        if (auto capability = input_capability(body["input"])) {
            require_capability(std::move(capability->first),
                               std::move(capability->second));
        }
    }
    if (body.contains("background") && body["background"].is_boolean() &&
        body["background"].get<bool>())
        require_capability("background", "background Responses");
    if (body.contains("store") && body["store"].is_boolean() &&
        body["store"].get<bool>())
        require_capability("store", "stored Responses");
    if (body.contains("conversation") && !body["conversation"].is_null())
        require_capability("conversation", "stateful Conversations");
    if (body.contains("previous_response_id") &&
        !body["previous_response_id"].is_null())
        require_capability("previous_response_id", "response continuation");
    if (body.contains("prompt") && !body["prompt"].is_null())
        require_capability("prompt", "hosted prompt templates");
    if (body.contains("context_management") &&
        body["context_management"].is_array() &&
        !body["context_management"].empty())
        require_capability("context_management", "context compaction");
    if (body.contains("moderation") && !body["moderation"].is_null())
        require_capability("moderation", "moderated Responses");
    if (body.contains("include") && body["include"].is_array()) {
        for (const auto& value : body["include"]) {
            if (value != "message.output_text.logprobs") {
                require_capability("include", "the requested include expansion");
                break;
            }
        }
    }
    if (body.contains("truncation") && body["truncation"] == "auto")
        require_capability("truncation", "automatic context truncation");
    if (body.contains("prompt_cache_retention") &&
        body["prompt_cache_retention"] == "24h")
        require_capability("prompt_cache_retention", "24-hour prompt caching");
    if (body.contains("prompt_cache_options") &&
        body["prompt_cache_options"].is_object() &&
        body["prompt_cache_options"].value("mode", std::string{"implicit"}) ==
            "explicit")
        require_capability("prompt_cache_options",
                           "explicit prompt-cache breakpoints");
    if (body.contains("service_tier") && body["service_tier"].is_string()) {
        const auto tier = body["service_tier"].get<std::string>();
        if (tier != "auto" && tier != "default")
            require_capability("service_tier", "the requested service tier");
    }
    if (body.contains("text") && body["text"].is_object() &&
        body["text"].contains("verbosity") &&
        !body["text"]["verbosity"].is_null())
        require_capability("text", "the requested text verbosity");
    if (unsupported_tool_type)
        require_capability("tools", "Responses tool type " +
                                        *unsupported_tool_type);
    if (unsupported_choice_type)
        require_capability("tool_choice", "Responses tool choice " +
                                              *unsupported_choice_type);
    return foundation::Ok(std::move(parsed));
}

}
