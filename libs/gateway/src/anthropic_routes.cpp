#include "gateway/anthropic_routes.hpp"

#include "foundation/logging.hpp"
#include "gateway/streaming_sanitizer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_set>

namespace inferdeck::gateway {

using namespace inferdeck::foundation;

namespace {

constexpr std::size_t max_pending_stream_deltas = 128;
constexpr std::size_t max_pending_stream_bytes = 1024 * 1024;

std::size_t delta_size(const model::InferenceDelta& delta) {
    std::size_t size = delta.content.size() + delta.reasoning_text.size();
    for (const auto& call : delta.tool_calls) {
        size += call.id.size() + call.type.size() + call.function_name.size() +
                call.function_arguments.size();
    }
    return size;
}

bool anthropic_uses_vision(const nlohmann::json& body) {
    if (!body.contains("messages") || !body["messages"].is_array()) return false;
    for (const auto& message : body["messages"]) {
        if (!message.is_object() || !message.contains("content") ||
            !message["content"].is_array()) {
            continue;
        }
        for (const auto& block : message["content"]) {
            if (block.is_object() && block.value("type", "") == "image") return true;
        }
    }
    return false;
}

std::string make_msg_id() {
    static std::mutex mtx;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lock(mtx);
    return "msg_" + std::to_string(rng());
}

std::string make_tool_id() {
    static std::mutex mtx;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lock(mtx);
    return "toolu_" + std::to_string(rng());
}

// Flatten Anthropic content (string or array of text blocks) to plain text.
std::string flatten_text(const nlohmann::json& content) {
    if (content.is_string()) return content.get<std::string>();
    std::string out;
    if (content.is_array()) {
        for (const auto& block : content) {
            if (block.value("type", "") == "text") {
                if (!out.empty()) out += "\n";
                out += block.value("text", "");
            }
        }
    }
    return out;
}

bool integer_in_range(const nlohmann::json& value, std::int64_t minimum,
                      std::int64_t maximum) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        const auto unsigned_minimum =
            minimum <= 0 ? std::uint64_t{0}
                         : static_cast<std::uint64_t>(minimum);
        return number >= unsigned_minimum &&
               number <= static_cast<std::uint64_t>(maximum);
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        return number >= minimum && number <= maximum;
    }
    return false;
}

foundation::Result<void> require_fields(
    const nlohmann::json& value,
    const std::unordered_set<std::string>& supported,
    const std::string& context) {
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        if (!supported.contains(iterator.key())) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "unsupported " + context + " parameter: " + iterator.key());
        }
    }
    return foundation::Ok();
}

foundation::Result<void> validate_cache_control(const nlohmann::json& value) {
    if (!value.is_object() || !value.contains("type") ||
        !value["type"].is_string() ||
        value["type"].get<std::string>() != "ephemeral" ||
        value.size() != 1) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "cache_control must be {\"type\":\"ephemeral\"}");
    }
    return foundation::Ok();
}

foundation::Result<void> validate_text_blocks(const nlohmann::json& content,
                                              const std::string& context) {
    if (!content.is_array() || content.empty()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            context + " must be a non-empty array");
    }
    static const std::unordered_set<std::string> fields = {
        "type", "text", "cache_control",
    };
    for (const auto& block : content) {
        if (!block.is_object()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                context + " blocks must be objects");
        }
        auto allowed = require_fields(block, fields, context + " text block");
        if (!allowed) return allowed;
        if (!block.contains("type") || !block["type"].is_string() ||
            block["type"].get<std::string>() != "text" ||
            !block.contains("text") || !block["text"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                context + " supports only text blocks with string text");
        }
        if (block.contains("cache_control")) {
            auto cache = validate_cache_control(block["cache_control"]);
            if (!cache) return cache;
        }
    }
    return foundation::Ok();
}

foundation::Result<void> validate_anthropic_request(
    const nlohmann::json& body, bool require_max_tokens) {
    if (!body.is_object()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "request body must be an object");
    }
    static const std::unordered_set<std::string> fields = {
        "model", "messages", "max_tokens", "system", "stream",
        "temperature", "top_p", "top_k", "stop_sequences", "tools",
        "tool_choice", "priority", "metadata",
    };
    auto allowed = require_fields(body, fields, "Messages");
    if (!allowed) return allowed;
    if (!body.contains("model") || !body["model"].is_string() ||
        body["model"].get<std::string>().empty()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "request body must include non-empty string 'model'");
    }
    if (!body.contains("messages") || !body["messages"].is_array() ||
        body["messages"].empty()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "request body must include non-empty 'messages' array");
    }
    if (require_max_tokens &&
        (!body.contains("max_tokens") ||
         !integer_in_range(body["max_tokens"], 1,
                           std::numeric_limits<int>::max()))) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "max_tokens must be a positive integer");
    }
    if (!require_max_tokens && body.contains("max_tokens")) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "max_tokens is not accepted by count_tokens");
    }
    if (body.contains("stream") && !body["stream"].is_boolean()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "stream must be a boolean");
    }
    if (body.contains("temperature")) {
        if (!body["temperature"].is_number()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "temperature must be a number");
        }
        const double value = body["temperature"].get<double>();
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "temperature must be between 0 and 1");
        }
    }
    if (body.contains("top_p")) {
        if (!body["top_p"].is_number()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "top_p must be a number");
        }
        const double value = body["top_p"].get<double>();
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "top_p must be between 0 and 1");
        }
    }
    if (body.contains("top_k") &&
        !integer_in_range(body["top_k"], 1,
                          std::numeric_limits<int>::max())) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "top_k must be a positive integer");
    }
    if (body.contains("priority") &&
        !integer_in_range(body["priority"], -100, 100)) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "priority must be an integer between -100 and 100");
    }
    if (body.contains("metadata") && !body["metadata"].is_object()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "metadata must be an object");
    }
    if (body.contains("stop_sequences")) {
        if (!body["stop_sequences"].is_array()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "stop_sequences must be an array");
        }
        for (const auto& stop : body["stop_sequences"]) {
            if (!stop.is_string() || stop.get<std::string>().empty()) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "stop_sequences entries must be non-empty strings");
            }
        }
    }
    if (body.contains("system")) {
        if (body["system"].is_array()) {
            auto system = validate_text_blocks(body["system"], "system");
            if (!system) return system;
        } else if (!body["system"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "system must be a string or text block array");
        }
    }

    static const std::unordered_set<std::string> message_fields = {
        "role", "content",
    };
    static const std::unordered_set<std::string> text_fields = {
        "type", "text", "cache_control",
    };
    static const std::unordered_set<std::string> image_fields = {
        "type", "source", "cache_control",
    };
    static const std::unordered_set<std::string> result_fields = {
        "type", "tool_use_id", "content", "is_error", "cache_control",
    };
    static const std::unordered_set<std::string> tool_use_fields = {
        "type", "id", "name", "input", "cache_control",
    };
    static const std::unordered_set<std::string> thinking_fields = {
        "type", "thinking", "signature",
    };

    for (const auto& message : body["messages"]) {
        if (!message.is_object()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "messages entries must be objects");
        }
        allowed = require_fields(message, message_fields, "message");
        if (!allowed) return allowed;
        if (!message.contains("role") || !message["role"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "message role must be a string");
        }
        const std::string role = message["role"].get<std::string>();
        if (role != "user" && role != "assistant") {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "message role must be user or assistant");
        }
        if (!message.contains("content")) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "message requires content");
        }
        const auto& content = message["content"];
        if (content.is_string()) continue;
        if (!content.is_array() || content.empty()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "message content must be a string or non-empty array");
        }
        int assistant_stage = 0;
        for (const auto& block : content) {
            if (!block.is_object() || !block.contains("type") ||
                !block["type"].is_string()) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "content blocks require string type");
            }
            const std::string type = block["type"].get<std::string>();
            if (type == "text") {
                allowed = require_fields(block, text_fields, "text block");
                if (!allowed) return allowed;
                if (!block.contains("text") || !block["text"].is_string()) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "text block requires string text");
                }
                if (role == "assistant" && assistant_stage > 1) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "assistant text blocks must precede tool_use blocks");
                }
                if (role == "assistant") assistant_stage = 1;
            } else if (type == "image") {
                if (role != "user") {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "image blocks are accepted only in user messages");
                }
                allowed = require_fields(block, image_fields, "image block");
                if (!allowed) return allowed;
                if (!block.contains("source") || !block["source"].is_object() ||
                    !block["source"].contains("type") ||
                    !block["source"]["type"].is_string()) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "image block requires source");
                }
                const auto& source = block["source"];
                const std::string source_type = source["type"].get<std::string>();
                if (source_type == "base64") {
                    static const std::unordered_set<std::string> source_fields = {
                        "type", "media_type", "data",
                    };
                    allowed = require_fields(source, source_fields, "image source");
                    if (!allowed) return allowed;
                    if (!source.contains("media_type") ||
                        !source["media_type"].is_string() ||
                        source["media_type"].get<std::string>().empty() ||
                        !source.contains("data") || !source["data"].is_string() ||
                        source["data"].get<std::string>().empty()) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "base64 image requires media_type and data strings");
                    }
                } else if (source_type == "url") {
                    static const std::unordered_set<std::string> source_fields = {
                        "type", "url",
                    };
                    allowed = require_fields(source, source_fields, "image source");
                    if (!allowed) return allowed;
                    if (!source.contains("url") || !source["url"].is_string() ||
                        source["url"].get<std::string>().empty()) {
                        return foundation::Err<void>(
                            foundation::ErrorCode::InvalidArgument,
                            "URL image requires non-empty url");
                    }
                } else {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "unsupported image source type: " + source_type);
                }
            } else if (type == "tool_result") {
                if (role != "user") {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "tool_result blocks are accepted only in user messages");
                }
                allowed = require_fields(block, result_fields, "tool_result block");
                if (!allowed) return allowed;
                if (!block.contains("tool_use_id") ||
                    !block["tool_use_id"].is_string() ||
                    block["tool_use_id"].get<std::string>().empty()) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "tool_result requires non-empty tool_use_id");
                }
                if (block.contains("content") &&
                    !block["content"].is_string()) {
                    auto result_content = validate_text_blocks(
                        block["content"], "tool_result content");
                    if (!result_content) return result_content;
                }
                if (block.contains("is_error") &&
                    !block["is_error"].is_boolean()) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "tool_result is_error must be a boolean");
                }
            } else if (type == "tool_use") {
                if (role != "assistant") {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "tool_use blocks are accepted only in assistant messages");
                }
                allowed = require_fields(block, tool_use_fields, "tool_use block");
                if (!allowed) return allowed;
                if (!block.contains("id") || !block["id"].is_string() ||
                    block["id"].get<std::string>().empty() ||
                    !block.contains("name") || !block["name"].is_string() ||
                    block["name"].get<std::string>().empty() ||
                    !block.contains("input") || !block["input"].is_object()) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "tool_use requires id, name, and object input");
                }
                assistant_stage = 2;
            } else if (type == "thinking") {
                if (role != "assistant" || assistant_stage != 0) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "thinking blocks must precede assistant text and tool_use blocks");
                }
                allowed = require_fields(block, thinking_fields, "thinking block");
                if (!allowed) return allowed;
                if (!block.contains("thinking") ||
                    !block["thinking"].is_string()) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "thinking block requires string thinking");
                }
                if (block.contains("signature") &&
                    !block["signature"].is_string()) {
                    return foundation::Err<void>(
                        foundation::ErrorCode::InvalidArgument,
                        "thinking signature must be a string");
                }
            } else {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "unsupported content block type: " + type);
            }
            if (block.contains("cache_control")) {
                auto cache = validate_cache_control(block["cache_control"]);
                if (!cache) return cache;
            }
        }
    }

    if (body.contains("tools")) {
        if (!body["tools"].is_array()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "tools must be an array");
        }
        static const std::unordered_set<std::string> tool_fields = {
            "name", "description", "input_schema", "cache_control",
        };
        for (const auto& tool : body["tools"]) {
            if (!tool.is_object()) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "tool definitions must be objects");
            }
            allowed = require_fields(tool, tool_fields, "tool");
            if (!allowed) return allowed;
            if (!tool.contains("name") || !tool["name"].is_string() ||
                tool["name"].get<std::string>().empty() ||
                !tool.contains("input_schema") ||
                !tool["input_schema"].is_object()) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "tool requires non-empty name and object input_schema");
            }
            if (tool.contains("description") &&
                !tool["description"].is_string()) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "tool description must be a string");
            }
            if (tool.contains("cache_control")) {
                auto cache = validate_cache_control(tool["cache_control"]);
                if (!cache) return cache;
            }
        }
    }
    if (body.contains("tool_choice")) {
        if (!body["tool_choice"].is_object()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "tool_choice must be an object");
        }
        static const std::unordered_set<std::string> choice_fields = {
            "type", "name", "disable_parallel_tool_use",
        };
        allowed = require_fields(body["tool_choice"], choice_fields,
                                 "tool_choice");
        if (!allowed) return allowed;
        if (!body["tool_choice"].contains("type") ||
            !body["tool_choice"]["type"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "tool_choice requires string type");
        }
        const std::string type =
            body["tool_choice"]["type"].get<std::string>();
        if (type != "auto" && type != "any" && type != "none" &&
            type != "tool") {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "unsupported tool_choice type: " + type);
        }
        if (type == "tool" &&
            (!body["tool_choice"].contains("name") ||
             !body["tool_choice"]["name"].is_string() ||
             body["tool_choice"]["name"].get<std::string>().empty())) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "tool tool_choice requires non-empty name");
        }
        if (type != "tool" && body["tool_choice"].contains("name")) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "tool_choice name is accepted only for type tool");
        }
        if (body["tool_choice"].contains("disable_parallel_tool_use") &&
            !body["tool_choice"]["disable_parallel_tool_use"].is_boolean()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "disable_parallel_tool_use must be a boolean");
        }
    }
    return foundation::Ok();
}

std::string sse_event(const std::string& name, const nlohmann::json& data) {
    return "event: " + name + "\ndata: " +
        data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n\n";
}

// Tracks Anthropic content-block state across streamed deltas so events are
// emitted in the required start/delta/stop sequence with consistent indices.
struct AnthropicBlockState {
    enum class Block { None, Text, Tool };
    Block open{Block::None};
    int index{-1};
    bool any_tool_block{false};

    std::string close_block() {
        if (open == Block::None) return {};
        open = Block::None;
        return sse_event("content_block_stop",
                         {{"type", "content_block_stop"}, {"index", index}});
    }

    std::string ensure_text_block() {
        if (open == Block::Text) return {};
        std::string out = close_block();
        ++index;
        open = Block::Text;
        out += sse_event("content_block_start", {
            {"type", "content_block_start"},
            {"index", index},
            {"content_block", {{"type", "text"}, {"text", ""}}},
        });
        return out;
    }

    std::string start_tool_block(const std::string& id, const std::string& name) {
        std::string out = close_block();
        ++index;
        open = Block::Tool;
        any_tool_block = true;
        out += sse_event("content_block_start", {
            {"type", "content_block_start"},
            {"index", index},
            {"content_block", {
                {"type", "tool_use"},
                {"id", id.empty() ? make_tool_id() : id},
                {"name", name},
                {"input", nlohmann::json::object()},
            }},
        });
        return out;
    }

    std::string text_delta(const std::string& text) {
        std::string out = ensure_text_block();
        out += sse_event("content_block_delta", {
            {"type", "content_block_delta"},
            {"index", index},
            {"delta", {{"type", "text_delta"}, {"text", text}}},
        });
        return out;
    }

    std::string input_json_delta(const std::string& partial_json) {
        std::string out;
        out += sse_event("content_block_delta", {
            {"type", "content_block_delta"},
            {"index", index},
            {"delta", {{"type", "input_json_delta"}, {"partial_json", partial_json}}},
        });
        return out;
    }

    std::string from_delta(const model::InferenceDelta& delta) {
        std::string out;
        if (!delta.content.empty()) out += text_delta(delta.content);
        for (const auto& tc : delta.tool_calls) {
            if (!tc.function_name.empty() || !tc.id.empty()) {
                out += start_tool_block(tc.id, tc.function_name);
            }
            if (!tc.function_arguments.empty()) {
                if (open != Block::Tool) {
                    // Arguments without a preceding name delta; open a block anyway.
                    out += start_tool_block(tc.id, tc.function_name);
                }
                out += input_json_delta(tc.function_arguments);
            }
        }
        return out;
    }

    // Emit complete tool_use blocks (used when tool calls only appear in the
    // final result, not in streamed deltas).
    std::string full_tool_blocks(const std::vector<model::ToolCall>& tool_calls) {
        std::string out;
        for (const auto& tc : tool_calls) {
            out += start_tool_block(tc.id, tc.function_name);
            out += input_json_delta(tc.function_arguments);
        }
        return out;
    }
};

nlohmann::json tool_use_block(const model::ToolCall& tc) {
    nlohmann::json input;
    try {
        input = nlohmann::json::parse(tc.function_arguments);
        if (!input.is_object()) input = nlohmann::json::object();
    } catch (...) {
        input = nlohmann::json::object();
    }
    return {
        {"type", "tool_use"},
        {"id", tc.id.empty() ? make_tool_id() : tc.id},
        {"name", tc.function_name},
        {"input", input},
    };
}

model::InferenceRequest make_inference_request_from(const nlohmann::json& openai_body) {
    model::InferenceRequest ir;
    ir.openai_body_json = openai_body.dump();
    ir.max_tokens = openai_body.value(
        "max_tokens", model::k_max_tokens_use_context_budget);
    // Only carry sampler params the client explicitly set; unset ones fall back
    // to the server-side SamplingConfig defaults downstream (issue #42).
    if (openai_body.contains("temperature") && !openai_body["temperature"].is_null())
        ir.temperature = openai_body["temperature"].get<float>();
    if (openai_body.contains("top_p") && !openai_body["top_p"].is_null())
        ir.top_p = openai_body["top_p"].get<float>();
    if (openai_body.contains("top_k") && !openai_body["top_k"].is_null())
        ir.top_k = openai_body["top_k"].get<int>();
    return ir;
}

struct ErrorClass {
    int status;
    std::string type;
};

ErrorClass classify_error(foundation::ErrorCode code) {
    const bool invalid = code == foundation::ErrorCode::InvalidArgument ||
                         code == foundation::ErrorCode::ParseError ||
                         code == foundation::ErrorCode::ContextLengthExceeded;
    if (invalid) return {400, "invalid_request_error"};
    return {500, "api_error"};
}

} // namespace

std::string resolve_anthropic_model(const GatewayDeps& deps, const std::string& requested) {
    auto alias = deps.anthropic_model_aliases.find(requested);
    if (alias != deps.anthropic_model_aliases.end()) {
        const auto resolved = deps.coordinator.registry().resolve(alias->second);
        return resolved ? *resolved : std::string{};
    }
    if (!requested.empty()) {
        const auto resolved = deps.coordinator.registry().resolve(requested);
        return resolved ? *resolved : std::string{};
    }
    auto loaded = deps.coordinator.get_loaded_model();
    if (!deps.default_model.empty() && deps.coordinator.registry().has(deps.default_model)) {
        // Prefer whatever is already loaded over forcing a swap to the default.
        if (loaded && deps.coordinator.registry().has(*loaded)) return *loaded;
        return deps.default_model;
    }
    if (loaded) return *loaded;
    return {};
}

std::string anthropic_stop_reason(const std::string& finish_reason, bool has_tool_calls) {
    if (has_tool_calls || finish_reason == "tool_calls") return "tool_use";
    if (finish_reason == "length") return "max_tokens";
    return "end_turn";
}

nlohmann::json anthropic_to_openai(const nlohmann::json& body, const std::string& resolved_model) {
    nlohmann::json out;
    out["model"] = resolved_model;
    auto messages = nlohmann::json::array();

    if (body.contains("system")) {
        const std::string sys = flatten_text(body["system"]);
        if (!sys.empty()) {
            messages.push_back({{"role", "system"}, {"content", sys}});
        }
    }

    for (const auto& msg : body.value("messages", nlohmann::json::array())) {
        const std::string role = msg.value("role", "user");
        const auto& content = msg.contains("content") ? msg["content"] : nlohmann::json("");

        if (content.is_string()) {
            messages.push_back({{"role", role}, {"content", content.get<std::string>()}});
            continue;
        }
        if (!content.is_array()) continue;

        if (role == "assistant") {
            nlohmann::json m = {{"role", "assistant"}};
            std::string text;
            std::string reasoning;
            auto tool_calls = nlohmann::json::array();
            for (const auto& block : content) {
                const std::string type = block.value("type", "");
                if (type == "text") {
                    text += block.value("text", "");
                } else if (type == "thinking") {
                    reasoning += block.value("thinking", "");
                } else if (type == "tool_use") {
                    tool_calls.push_back({
                        {"id", block.value("id", "")},
                        {"type", "function"},
                        {"function", {
                            {"name", block.value("name", "")},
                            {"arguments", block.value("input", nlohmann::json::object()).dump()},
                        }},
                    });
                }
            }
            m["content"] = text;
            if (!reasoning.empty()) m["reasoning_content"] = reasoning;
            if (!tool_calls.empty()) m["tool_calls"] = tool_calls;
            messages.push_back(std::move(m));
            continue;
        }

        auto parts = nlohmann::json::array();
        bool has_image = false;
        auto flush_user = [&messages, &parts, &has_image]() {
            if (parts.empty()) return;
            if (!has_image) {
                std::string text;
                for (const auto& part : parts) {
                    text += part["text"].get<std::string>();
                }
                messages.push_back({{"role", "user"}, {"content", std::move(text)}});
            } else {
                messages.push_back({{"role", "user"}, {"content", std::move(parts)}});
            }
            parts = nlohmann::json::array();
            has_image = false;
        };
        for (const auto& block : content) {
            const std::string type = block.value("type", "");
            if (type == "tool_result") {
                flush_user();
                std::string result = flatten_text(
                    block.contains("content") ? block["content"]
                                              : nlohmann::json(""));
                if (block.value("is_error", false)) {
                    result = "Error: " + result;
                }
                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", block.value("tool_use_id", "")},
                    {"content", std::move(result)},
                });
            } else if (type == "text") {
                parts.push_back({{"type", "text"}, {"text", block.value("text", "")}});
            } else if (type == "image") {
                const auto& src = block.value("source", nlohmann::json::object());
                if (src.value("type", "") == "base64") {
                    parts.push_back({
                        {"type", "image_url"},
                        {"image_url", {{"url", "data:" + src.value("media_type", "image/png") +
                                               ";base64," + src.value("data", "")}}},
                    });
                } else if (src.value("type", "") == "url") {
                    parts.push_back({
                        {"type", "image_url"},
                        {"image_url", {{"url", src.value("url", "")}}},
                    });
                }
                has_image = true;
            }
        }
        flush_user();
    }
    out["messages"] = std::move(messages);

    if (body.contains("tools") && body["tools"].is_array() && !body["tools"].empty()) {
        auto tools = nlohmann::json::array();
        for (const auto& t : body["tools"]) {
            tools.push_back({
                {"type", "function"},
                {"function", {
                    {"name", t.value("name", "")},
                    {"description", t.value("description", "")},
                    {"parameters", t.value("input_schema", nlohmann::json::object())},
                }},
            });
        }
        out["tools"] = std::move(tools);
    }
    if (body.contains("tool_choice") && body["tool_choice"].is_object()) {
        const std::string tc_type = body["tool_choice"].value("type", "auto");
        if (tc_type == "any") {
            out["tool_choice"] = "required";
        } else if (tc_type == "tool") {
            out["tool_choice"] = {
                {"type", "function"},
                {"function", {{"name", body["tool_choice"].value("name", "")}}},
            };
        } else if (tc_type == "none") {
            out["tool_choice"] = "none";
        } else {
            out["tool_choice"] = "auto";
        }
        if (body["tool_choice"].value("disable_parallel_tool_use", false)) {
            out["parallel_tool_calls"] = false;
        }
    }

    if (body.contains("max_tokens")) out["max_tokens"] = body["max_tokens"];
    if (body.contains("temperature")) out["temperature"] = body["temperature"];
    if (body.contains("top_p")) out["top_p"] = body["top_p"];
    if (body.contains("top_k")) out["top_k"] = body["top_k"];
    if (body.contains("stop_sequences")) out["stop"] = body["stop_sequences"];
    out["stream"] = body.value("stream", false);
    return out;
}

void write_anthropic_error(httplib::Response& resp, int status,
                           const std::string& type, const std::string& message) {
    nlohmann::json body = {
        {"type", "error"},
        {"error", {{"type", type}, {"message", message}}},
    };
    write_json(resp, status, body);
}

void handle_anthropic_count_tokens(const httplib::Request& req, httplib::Response& resp,
                                   const GatewayDeps& deps) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
        write_anthropic_error(resp, 400, "invalid_request_error", "invalid JSON");
        return;
    }
    auto validation = validate_anthropic_request(body, false);
    if (!validation) {
        write_anthropic_error(resp, 400, "invalid_request_error",
                              validation.error().message);
        return;
    }
    const std::string requested_model = body["model"].get<std::string>();
    if (resolve_anthropic_model(deps, requested_model).empty()) {
        write_anthropic_error(resp, 404, "not_found_error",
                              "no model available for: " + requested_model);
        return;
    }
    std::size_t chars = 0;
    if (body.contains("system")) chars += flatten_text(body["system"]).size();
    if (body.contains("messages")) chars += body["messages"].dump().size();
    if (body.contains("tools")) chars += body["tools"].dump().size();
    write_json(resp, 200, {{"input_tokens", static_cast<int>(chars / 4) + 1}});
}

void handle_anthropic_messages(const httplib::Request& req, httplib::Response& resp,
                               const GatewayDeps& deps) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
        write_anthropic_error(resp, 400, "invalid_request_error", "invalid JSON");
        return;
    }
    auto validation = validate_anthropic_request(body, true);
    if (!validation) {
        write_anthropic_error(resp, 400, "invalid_request_error",
                              validation.error().message);
        return;
    }

    const std::string requested_model = body.value("model", "");
    const std::string model_name = resolve_anthropic_model(deps, requested_model);
    if (model_name.empty()) {
        write_anthropic_error(resp, 404, "not_found_error",
                              "no model available for: " + requested_model);
        return;
    }
    if (maintenance_blocks_model(deps, model_name)) {
        write_anthropic_error(resp, 503, "overloaded_error",
                              "measured model optimization is using the same compute resource");
        return;
    }
    const auto model_info = deps.coordinator.registry().get_info_result(model_name);
    if (model_info && !model_info->has_vision && anthropic_uses_vision(body)) {
        write_anthropic_error(resp, 400, "invalid_request_error",
                              "model does not support image input: " + model_name);
        return;
    }

    nlohmann::json openai_body;
    try {
        openai_body = anthropic_to_openai(body, model_name);
    } catch (const std::exception& e) {
        write_anthropic_error(resp, 400, "invalid_request_error", e.what());
        return;
    }
    model::InferenceRequest ir;
    try {
        ir = make_inference_request_from(openai_body);
    } catch (const std::exception& error) {
        write_anthropic_error(resp, 400, "invalid_request_error", error.what());
        return;
    }

    const bool stream = body.value("stream", false);

    int slot_id = -1;
    {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::minutes{5};
        const std::function<bool()> cancelled = [&req] {
            return req.is_connection_closed();
        };
        model::AcquireSlotOptions opts;
        opts.timeout = std::chrono::minutes{5};
        opts.block = true;
        opts.priority = body.contains("priority") && body["priority"].is_number_integer()
            ? std::clamp(body["priority"].get<int>(), -100, 100) : 0;
        opts.cancelled = cancelled;
        opts.prepare = [&deps, &model_name, deadline, cancelled] {
            auto loaded = ensure_model_loaded(
                deps, model_name, deadline, cancelled);
            if (loaded.ok) return foundation::Ok();
            return foundation::Err<void>(loaded.error_code, loaded.message);
        };
        auto sr = deps.coordinator.acquire_slot(model_name, opts);
        if (!sr) {
            resp.set_header("Retry-After",
                sr.error().code == foundation::ErrorCode::NotLoaded
                    ? deps.default_swap_timeout_s : "1");
            write_anthropic_error(resp, 503, "overloaded_error", sr.error().message);
            return;
        }
        slot_id = *sr;
    }

    const std::string id = make_msg_id();
    if (!stream) {
        model::InferenceResult pr;
        {
            struct SlotGuard {
                const GatewayDeps& deps;
                const std::string& model;
                int slot;
                ~SlotGuard() { (void)deps.coordinator.release_slot(model, slot); }
            } guard{deps, model_name, slot_id};

            auto pres = deps.coordinator.predict(model_name, slot_id, ir);
            if (!pres) {
                const auto ec = classify_error(pres.error().code);
                LOG_ERROR("anthropic_inference_failed", "model={} slot_id={} error={}",
                          model_name, slot_id, pres.error().message);
                model::InferenceResult failed;
                record_request(deps, requested_model.empty() ? model_name : requested_model,
                               failed, ec.status, slot_id, 0.0, 0, model_name);
                write_anthropic_error(resp, ec.status, ec.type, pres.error().message);
                return;
            }
            pr = std::move(*pres);
        }
        record_request(deps, requested_model.empty() ? model_name : requested_model,
                       pr, 200, slot_id, 0.0, 0, model_name);

        auto content = nlohmann::json::array();
        if (!pr.text.empty()) {
            content.push_back({{"type", "text"}, {"text", pr.text}});
        }
        for (const auto& tc : pr.tool_calls) {
            content.push_back(tool_use_block(tc));
        }
        nlohmann::json resp_body = {
            {"id", id},
            {"type", "message"},
            {"role", "assistant"},
            {"model", requested_model.empty() ? model_name : requested_model},
            {"content", content},
            {"stop_reason", anthropic_stop_reason(pr.finish_reason, !pr.tool_calls.empty())},
            {"stop_sequence", nullptr},
            {"usage", {
                {"input_tokens", pr.prompt_tokens},
                {"output_tokens", pr.completion_tokens},
                {"cache_read_input_tokens", pr.cached_prompt_tokens},
                {"cache_creation_input_tokens", 0},
            }},
        };
        write_json(resp, 200, resp_body);
        return;
    }

    resp.set_header("Cache-Control", "no-cache");
    resp.set_header("Connection", "keep-alive");

    struct StreamState {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<model::InferenceDelta> delta_queue;
        std::size_t pending_bytes{0};
        bool inference_done{false};
        bool inference_error{false};
        foundation::ErrorCode error_code{foundation::ErrorCode::Internal};
        std::string error_msg;
        std::shared_ptr<model::InferenceResult> final_result;
        std::atomic<bool> aborted{false};
        std::thread inference_thread;
        int slot_id{-1};
        std::string model_name;
        std::string requested_model;
        model::BackendCoordinator* coordinator{nullptr};
        observability::Metrics* metrics{nullptr};
        observability::StatsDb* stats_db{nullptr};
        foundation::EventBus* events{nullptr};
        std::atomic<bool> cleanup_done{false};

        // Serialization state for the chunked provider (single-threaded use).
        AnthropicBlockState blocks;
        InferenceDeltaUtf8Buffer utf8;
        bool sent_message_start{false};
        bool streamed_any_tool{false};

        void finish_once(bool aborted_stream, const std::string& reason) {
            bool expected = false;
            if (!cleanup_done.compare_exchange_strong(expected, true)) return;
            if (aborted_stream) aborted.store(true);
            cv.notify_all();
            if (inference_thread.joinable()) {
                if (inference_thread.get_id() == std::this_thread::get_id()) {
                    inference_thread.detach();
                } else {
                    inference_thread.join();
                }
            }
            std::shared_ptr<model::InferenceResult> result;
            bool error = false;
            {
                std::lock_guard<std::mutex> lk(mtx);
                result = final_result;
                error = inference_error;
            }
            if (result && !aborted_stream && !error) {
                record_request(metrics, stats_db, events, requested_model, *result, 200, slot_id,
                               0.0, 0, model_name);
            } else {
                model::InferenceResult failed;
                record_request(metrics, stats_db, events, requested_model, failed,
                               aborted_stream ? 499 : 500, slot_id, 0.0, 0, model_name);
            }
            if (coordinator) {
                (void)coordinator->release_slot(model_name, slot_id);
            }
            LOG_INFO("anthropic_stream_cleanup", "model={} slot_id={} aborted={} reason={}",
                     model_name, slot_id, aborted_stream, reason);
        }
    };

    auto state = std::make_shared<StreamState>();
    state->slot_id = slot_id;
    state->model_name = model_name;
    state->requested_model = requested_model.empty() ? model_name : requested_model;
    state->coordinator = &deps.coordinator;
    state->metrics = deps.metrics;
    state->stats_db = deps.stats_db;
    state->events = deps.events;

    try {
        state->inference_thread = std::thread([state, ir]() {
            try {
                auto result = state->coordinator->predict_stream(
                    state->model_name, state->slot_id, ir,
                    [state](const model::InferenceDelta& delta) -> bool {
                        if (state->aborted.load()) return false;
                        {
                            const auto bytes = delta_size(delta);
                            std::unique_lock<std::mutex> lk(state->mtx);
                            state->cv.wait(lk, [state, bytes] {
                                const bool byte_capacity =
                                    state->delta_queue.empty() ||
                                    (bytes <= max_pending_stream_bytes &&
                                     state->pending_bytes <=
                                         max_pending_stream_bytes - bytes);
                                return state->aborted.load() ||
                                       (state->delta_queue.size() <
                                            max_pending_stream_deltas &&
                                        byte_capacity);
                            });
                            if (state->aborted.load()) return false;
                            state->delta_queue.push_back(delta);
                            state->pending_bytes += bytes;
                        }
                        state->cv.notify_one();
                        return !state->aborted.load();
                    },
                    &state->aborted);
                {
                    std::lock_guard<std::mutex> lk(state->mtx);
                    if (result) {
                        state->final_result =
                            std::make_shared<model::InferenceResult>(std::move(*result));
                    } else {
                        state->inference_error = true;
                        state->error_code = result.error().code;
                        state->error_msg = result.error().message;
                    }
                    state->inference_done = true;
                }
                state->cv.notify_all();
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(state->mtx);
                state->inference_error = true;
                state->error_msg = e.what();
                state->inference_done = true;
                state->cv.notify_all();
            } catch (...) {
                std::lock_guard<std::mutex> lk(state->mtx);
                state->inference_error = true;
                state->error_msg = "unknown exception";
                state->inference_done = true;
                state->cv.notify_all();
            }
        });
    } catch (const std::exception& error) {
        (void)deps.coordinator.release_slot(model_name, slot_id);
        LOG_ERROR("anthropic_stream_start_failed", "model={} slot_id={} what={}",
                  model_name, slot_id, error.what());
        write_anthropic_error(resp, 500, "api_error",
                              "stream worker could not be started");
        return;
    }

    const std::string reported_model = requested_model.empty() ? model_name : requested_model;

    resp.set_chunked_content_provider(
        "text/event-stream",
        [id, reported_model, state](std::size_t, httplib::DataSink& sink) mutable {
            try {
                auto write = [&](const std::string& data) -> bool {
                    return data.empty() || sink.write(data.data(), data.size());
                };

                if (!state->sent_message_start) {
                    state->sent_message_start = true;
                    nlohmann::json start = {
                        {"type", "message_start"},
                        {"message", {
                            {"id", id},
                            {"type", "message"},
                            {"role", "assistant"},
                            {"model", reported_model},
                            {"content", nlohmann::json::array()},
                            {"stop_reason", nullptr},
                            {"stop_sequence", nullptr},
                            {"usage", {{"input_tokens", 0}, {"output_tokens", 0}}},
                        }},
                    };
                    if (!write(sse_event("message_start", start))) {
                        state->finish_once(true, "message_start_write_failed");
                        return false;
                    }
                }

                std::unique_lock<std::mutex> lk(state->mtx);
                while (!state->cv.wait_for(lk, std::chrono::seconds{2}, [&] {
                    return !state->delta_queue.empty() || state->inference_done ||
                           state->aborted.load();
                })) {
                    lk.unlock();
                    if (!write(sse_event("ping", {{"type", "ping"}}))) {
                        state->finish_once(true, "heartbeat_write_failed");
                        return false;
                    }
                    lk.lock();
                }

                if (state->aborted.load() && !state->inference_done &&
                    state->delta_queue.empty()) {
                    lk.unlock();
                    state->finish_once(true, "aborted");
                    return false;
                }

                if (!state->delta_queue.empty()) {
                    std::deque<model::InferenceDelta> deltas;
                    while (!state->delta_queue.empty()) {
                        state->pending_bytes -= delta_size(state->delta_queue.front());
                        deltas.push_back(std::move(state->delta_queue.front()));
                        state->delta_queue.pop_front();
                    }
                    lk.unlock();
                    state->cv.notify_all();

                    for (const auto& delta : deltas) {
                        auto clean = state->utf8.on_delta(delta);
                        std::string out = state->blocks.from_delta(clean);
                        if (!clean.tool_calls.empty()) state->streamed_any_tool = true;
                        if (!write(out)) {
                            state->finish_once(true, "chunk_write_failed");
                            return false;
                        }
                    }
                    return true;
                }

                const bool inference_error = state->inference_error;
                const std::string error_msg = state->error_msg;
                const auto final_result = state->final_result;
                lk.unlock();

                auto trailing = state->utf8.finish();
                if (!trailing.content.empty() || !trailing.reasoning_text.empty() ||
                    !trailing.tool_calls.empty()) {
                    if (!trailing.tool_calls.empty()) state->streamed_any_tool = true;
                    if (!write(state->blocks.from_delta(trailing))) {
                        state->finish_once(true, "trailing_chunk_write_failed");
                        return false;
                    }
                }

                if (inference_error) {
                    LOG_ERROR("anthropic_inference_failed", "model={} slot_id={} error={}",
                              state->model_name, state->slot_id, error_msg);
                    const auto error_class =
                        classify_error(state->error_code);
                    nlohmann::json err = {
                        {"type", "error"},
                        {"error", {{"type", error_class.type},
                                   {"message", error_msg}}},
                    };
                    if (!write(sse_event("error", err))) {
                        state->finish_once(true, "error_write_failed");
                        return false;
                    }
                    state->finish_once(false, "inference_error");
                } else {
                    std::string tail;
                    const bool has_tool_calls = final_result && !final_result->tool_calls.empty();
                    if (has_tool_calls && !state->streamed_any_tool) {
                        tail += state->blocks.full_tool_blocks(final_result->tool_calls);
                    }
                    tail += state->blocks.close_block();
                    const std::string stop_reason = anthropic_stop_reason(
                        final_result ? final_result->finish_reason : "stop", has_tool_calls);
                    tail += sse_event("message_delta", {
                        {"type", "message_delta"},
                        {"delta", {{"stop_reason", stop_reason}, {"stop_sequence", nullptr}}},
                        {"usage", {
                            {"input_tokens", final_result ? final_result->prompt_tokens : 0},
                            {"output_tokens", final_result ? final_result->completion_tokens : 0},
                        }},
                    });
                    tail += sse_event("message_stop", {{"type", "message_stop"}});
                    if (!write(tail)) {
                        state->finish_once(true, "done_write_failed");
                        return false;
                    }
                    state->finish_once(false, "completed");
                }
                sink.done();
                return false;
            } catch (const std::exception& e) {
                LOG_ERROR("anthropic_stream_exception", "model={} slot_id={} what={}",
                          state->model_name, state->slot_id, e.what());
                state->finish_once(true, "provider_exception");
                return false;
            } catch (...) {
                state->finish_once(true, "provider_unknown_exception");
                return false;
            }
        },
        [state](bool success) {
            state->finish_once(!success, "resource_releaser");
        });
}

} // namespace inferdeck::gateway
