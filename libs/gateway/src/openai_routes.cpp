#include "gateway/openai_routes.hpp"
#include "gateway/generation_session.hpp"
#include "gateway/openai_adapter.hpp"
#include "gateway/openai_error.hpp"
#include "gateway/responses_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace inferdeck::gateway {

namespace {

std::string make_id(const char* prefix) {
    static std::mutex mutex;
    static std::mt19937_64 random{std::random_device{}()};
    std::lock_guard lock(mutex);
    return std::string(prefix) + std::to_string(random());
}

std::string base64_floats(const std::vector<float>& values) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes;
    bytes.reserve(values.size() * sizeof(float));
    for (const float value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        bytes.push_back(static_cast<unsigned char>(bits & 0xff));
        bytes.push_back(static_cast<unsigned char>((bits >> 8) & 0xff));
        bytes.push_back(static_cast<unsigned char>((bits >> 16) & 0xff));
        bytes.push_back(static_cast<unsigned char>((bits >> 24) & 0xff));
    }
    const std::size_t size = bytes.size();
    std::string output;
    output.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t chunk = static_cast<std::uint32_t>(bytes[i]) << 16 |
            static_cast<std::uint32_t>(i + 1 < size ? bytes[i + 1] : 0) << 8 |
            static_cast<std::uint32_t>(i + 2 < size ? bytes[i + 2] : 0);
        output.push_back(alphabet[(chunk >> 18) & 0x3f]);
        output.push_back(alphabet[(chunk >> 12) & 0x3f]);
        output.push_back(i + 1 < size ? alphabet[(chunk >> 6) & 0x3f] : '=');
        output.push_back(i + 2 < size ? alphabet[chunk & 0x3f] : '=');
    }
    return output;
}

foundation::Result<int> acquire_for_request(
    const httplib::Request& req, const GatewayDeps& deps,
    const std::string& model_name, int priority) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::minutes{5};
    const std::function<bool()> cancelled = [&req] {
        return req.is_connection_closed();
    };
    model::AcquireSlotOptions options;
    options.timeout = std::chrono::minutes{5};
    options.priority = std::clamp(priority, -100, 100);
    options.cancelled = cancelled;
    options.prepare = [&deps, &model_name, deadline, cancelled] {
        auto loaded = ensure_model_loaded(
            deps, model_name, deadline, cancelled);
        if (loaded.ok) return foundation::Ok();
        return foundation::Err<void>(loaded.error_code, loaded.message);
    };
    return deps.coordinator.acquire_slot(model_name, options);
}

bool response_input_uses_vision(const nlohmann::json& input) {
    if (input.is_array()) {
        return std::any_of(input.begin(), input.end(), [](const auto& item) {
            return response_input_uses_vision(item);
        });
    }
    if (!input.is_object()) return false;
    const auto type = input.contains("type") && input["type"].is_string()
        ? input["type"].get<std::string>() : std::string{};
    if (type == "image" || type == "image_url" || type == "input_image") return true;
    return input.contains("content") &&
           response_input_uses_vision(input["content"]);
}

nlohmann::json response_usage(const nlohmann::json& chat_usage) {
    const int input = chat_usage.value("prompt_tokens", 0);
    const int output = chat_usage.value("completion_tokens", 0);
    const int cached = chat_usage.value("prompt_tokens_details", nlohmann::json::object())
        .value("cached_tokens", 0);
    return {
        {"input_tokens", input},
        {"input_tokens_details", {{"cached_tokens", cached}}},
        {"output_tokens", output},
        {"output_tokens_details", {{"reasoning_tokens", 0}}},
        {"total_tokens", input + output},
    };
}

bool responses_request_wants_logprobs(const nlohmann::json& request) {
    if (request.contains("top_logprobs") &&
        !request["top_logprobs"].is_null()) {
        return true;
    }
    return request.contains("include") && request["include"].is_array() &&
        std::find(request["include"].begin(), request["include"].end(),
                  "message.output_text.logprobs") != request["include"].end();
}

nlohmann::json responses_logprobs(
    const std::vector<inference::TokenLogprob>& values, bool include_bytes) {
    nlohmann::json output = nlohmann::json::array();
    for (const auto& value : values) {
        nlohmann::json top = nlohmann::json::array();
        for (const auto& candidate : value.top_logprobs) {
            nlohmann::json item{
                {"token", candidate.token}, {"logprob", candidate.logprob},
            };
            if (include_bytes) item["bytes"] = candidate.bytes;
            top.push_back(std::move(item));
        }
        nlohmann::json item{
            {"token", value.token}, {"logprob", value.logprob},
            {"top_logprobs", std::move(top)},
        };
        if (include_bytes) item["bytes"] = value.bytes;
        output.push_back(std::move(item));
    }
    return output;
}


nlohmann::json result_to_response(const model::InferenceResult& result,
                                  const nlohmann::json& request,
                                  const std::string& response_id,
                                  const std::string& model_name,
                                  std::int64_t created_at) {
    nlohmann::json output = nlohmann::json::array();
    if (!result.reasoning_text.empty()) {
        output.push_back({
            {"id", make_id("rs_")}, {"type", "reasoning"},
            {"status", "completed"}, {"summary", nlohmann::json::array()},
            {"content", nlohmann::json::array({{
                {"type", "reasoning_text"}, {"text", result.reasoning_text},
            }})},
        });
    }
    if (!result.text.empty()) {
        nlohmann::json content{
            {"type", "output_text"}, {"text", result.text},
            {"annotations", nlohmann::json::array()},
        };
        if (responses_request_wants_logprobs(request)) {
            content["logprobs"] = responses_logprobs(result.logprobs, true);
        }
        output.push_back({
            {"id", make_id("msg_")}, {"type", "message"},
            {"status", "completed"}, {"role", "assistant"},
            {"content", nlohmann::json::array({std::move(content)})},
        });
    }
    for (const auto& tool : result.tool_calls) {
        output.push_back({
            {"id", make_id("fc_")}, {"type", "function_call"},
            {"status", "completed"},
            {"call_id", tool.id.empty() ? make_id("call_") : tool.id},
            {"name", tool.function_name},
            {"arguments", tool.function_arguments},
        });
    }
    auto response = nlohmann::json{
        {"id", response_id}, {"object", "response"},
        {"created_at", created_at}, {"status", "completed"},
        {"completed_at", static_cast<std::int64_t>(std::time(nullptr))},
        {"output_text", result.text},
        {"background", false}, {"conversation", nullptr},
        {"error", nullptr}, {"incomplete_details", nullptr},
        {"instructions", request.value("instructions", nlohmann::json(nullptr))},
        {"max_output_tokens", request.value("max_output_tokens", nlohmann::json(nullptr))},
        {"model", model_name}, {"output", output},
        {"parallel_tool_calls", request.value("parallel_tool_calls", true)},
        {"previous_response_id", nullptr},
        {"reasoning", request.value("reasoning", nlohmann::json::object())},
        {"service_tier", "default"}, {"store", false},
        {"text", request.value("text", nlohmann::json{{"format", {{"type", "text"}}}})},
        {"metadata", request.value("metadata", nlohmann::json::object())},
        {"temperature", request.value("temperature", 1.0)},
        {"top_p", request.value("top_p", 1.0)},
        {"tools", request.value("tools", nlohmann::json::array())},
        {"tool_choice", request.value("tool_choice", nlohmann::json("auto"))},
        {"truncation", "disabled"},
        {"usage", {
            {"input_tokens", result.prompt_tokens},
            {"input_tokens_details", {{"cached_tokens", result.cached_prompt_tokens}}},
            {"output_tokens", result.completion_tokens},
            {"output_tokens_details", {{"reasoning_tokens", 0}}},
            {"total_tokens", result.prompt_tokens + result.completion_tokens},
        }},
    };
    return response;
}

struct ToolStreamState {
    std::size_t source_index{0};
    int output_index{0};
    std::string item_id;
    std::string call_id;
    std::string name;
    std::string arguments;
};

struct ResponsesStreamState {
    std::shared_ptr<GenerationSession> session;
    nlohmann::json request;
    std::string response_id;
    std::string model;
    std::int64_t created_at{0};
    std::string message_id;
    std::string reasoning_id;
    std::optional<int> message_index;
    std::optional<int> reasoning_index;
    int next_output_index{0};
    int sequence{0};
    bool started{false};
    bool completed{false};
    bool include_obfuscation{true};
    std::string text;
    std::string reasoning;
    std::vector<inference::TokenLogprob> logprobs;
    std::map<std::size_t, ToolStreamState> tools;
    nlohmann::json usage = nlohmann::json::object();
};

nlohmann::json stream_response_object(const ResponsesStreamState& state,
                                      const std::string& status,
                                      nlohmann::json output,
                                      nlohmann::json error = nullptr) {
    auto response = nlohmann::json{
        {"id", state.response_id}, {"object", "response"},
        {"created_at", state.created_at}, {"status", status},
        {"output_text", state.text},
        {"background", false}, {"conversation", nullptr},
        {"error", error}, {"incomplete_details", nullptr},
        {"instructions", state.request.value("instructions", nlohmann::json(nullptr))},
        {"max_output_tokens", state.request.value("max_output_tokens", nlohmann::json(nullptr))},
        {"model", state.model}, {"output", std::move(output)},
        {"parallel_tool_calls", state.request.value("parallel_tool_calls", true)},
        {"previous_response_id", nullptr},
        {"reasoning", state.request.value("reasoning", nlohmann::json::object())},
        {"service_tier", "default"}, {"store", false},
        {"text", state.request.value("text", nlohmann::json{{"format", {{"type", "text"}}}})},
        {"metadata", state.request.value("metadata", nlohmann::json::object())},
        {"temperature", state.request.value("temperature", 1.0)},
        {"top_p", state.request.value("top_p", 1.0)},
        {"tools", state.request.value("tools", nlohmann::json::array())},
        {"tool_choice", state.request.value("tool_choice", nlohmann::json("auto"))},
        {"truncation", "disabled"},
        {"usage", response_usage(state.usage)},
    };
    if (status == "completed") {
        response["completed_at"] =
            static_cast<std::int64_t>(std::time(nullptr));
    }
    return response;
}

bool emit_response_event(ResponsesStreamState& state, httplib::DataSink& sink,
                         nlohmann::json event) {
    event["sequence_number"] = state.sequence++;
    if (state.include_obfuscation) {
        event["obfuscation"] = make_id("obf_").substr(4);
    }
    const std::string type = event.value("type", "response.event");
    const std::string frame = "event: " + type + "\ndata: " +
        event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n\n";
    return sink.write(frame.data(), frame.size());
}

bool start_response_stream(ResponsesStreamState& state, httplib::DataSink& sink) {
    if (state.started) return true;
    state.started = true;
    auto response = stream_response_object(state, "in_progress", nlohmann::json::array());
    return emit_response_event(state, sink, {{"type", "response.created"}, {"response", response}}) &&
           emit_response_event(state, sink, {{"type", "response.in_progress"}, {"response", response}});
}

bool ensure_message_stream(ResponsesStreamState& state, httplib::DataSink& sink) {
    if (state.message_index) return true;
    state.message_index = state.next_output_index++;
    state.message_id = make_id("msg_");
    const nlohmann::json item = {
        {"id", state.message_id}, {"type", "message"}, {"status", "in_progress"},
        {"role", "assistant"}, {"content", nlohmann::json::array()},
    };
    return emit_response_event(state, sink, {
        {"type", "response.output_item.added"}, {"output_index", *state.message_index},
        {"item", item},
    }) && emit_response_event(state, sink, {
        {"type", "response.content_part.added"}, {"item_id", state.message_id},
        {"output_index", *state.message_index}, {"content_index", 0},
        {"part", {{"type", "output_text"}, {"text", ""},
                  {"annotations", nlohmann::json::array()}}},
    });
}

bool ensure_reasoning_stream(ResponsesStreamState& state, httplib::DataSink& sink) {
    if (state.reasoning_index) return true;
    state.reasoning_index = state.next_output_index++;
    state.reasoning_id = make_id("rs_");
    const nlohmann::json item = {
        {"id", state.reasoning_id}, {"type", "reasoning"}, {"status", "in_progress"},
        {"summary", nlohmann::json::array()}, {"content", nlohmann::json::array()},
    };
    return emit_response_event(state, sink, {
        {"type", "response.output_item.added"}, {"output_index", *state.reasoning_index},
        {"item", item},
    }) && emit_response_event(state, sink, {
        {"type", "response.content_part.added"}, {"item_id", state.reasoning_id},
        {"output_index", *state.reasoning_index}, {"content_index", 0},
        {"part", {{"type", "reasoning_text"}, {"text", ""}}},
    });
}

bool apply_sanitized_generation_delta(ResponsesStreamState& state,
                                      httplib::DataSink& sink,
                                      const model::InferenceDelta& delta) {
    if (!delta.reasoning_text.empty()) {
        if (!ensure_reasoning_stream(state, sink)) return false;
        state.reasoning += delta.reasoning_text;
        if (!emit_response_event(state, sink, {
            {"type", "response.reasoning_text.delta"},
            {"item_id", state.reasoning_id},
            {"output_index", *state.reasoning_index}, {"content_index", 0},
            {"delta", delta.reasoning_text},
        })) return false;
    }
    if (!delta.content.empty()) {
        if (!ensure_message_stream(state, sink)) return false;
        state.text += delta.content;
        state.logprobs.insert(state.logprobs.end(), delta.logprobs.begin(),
                              delta.logprobs.end());
        if (!emit_response_event(state, sink, {
            {"type", "response.output_text.delta"},
            {"item_id", state.message_id},
            {"output_index", *state.message_index}, {"content_index", 0},
            {"delta", delta.content},
            {"logprobs", responses_logprobs(delta.logprobs, false)},
        })) return false;
    }
    for (const auto& source : delta.tool_calls) {
        auto [iterator, inserted] = state.tools.try_emplace(source.index);
        auto& tool = iterator->second;
        if (inserted) {
            tool.source_index = source.index;
            tool.output_index = state.next_output_index++;
            tool.item_id = make_id("fc_");
            tool.call_id = source.id.empty() ? make_id("call_") : source.id;
            tool.name = source.function_name;
            const nlohmann::json item = {
                {"id", tool.item_id}, {"type", "function_call"},
                {"status", "in_progress"}, {"call_id", tool.call_id},
                {"name", tool.name}, {"arguments", ""},
            };
            if (!emit_response_event(state, sink, {
                {"type", "response.output_item.added"},
                {"output_index", tool.output_index}, {"item", item},
            })) return false;
        } else {
            if (!source.id.empty()) tool.call_id = source.id;
            if (!source.function_name.empty()) tool.name += source.function_name;
        }
        if (!source.function_arguments.empty()) {
            tool.arguments += source.function_arguments;
            if (!emit_response_event(state, sink, {
                {"type", "response.function_call_arguments.delta"},
                {"item_id", tool.item_id},
                {"output_index", tool.output_index},
                {"delta", source.function_arguments},
            })) return false;
        }
    }
    return true;
}

bool apply_generation_delta(ResponsesStreamState& state,
                            httplib::DataSink& sink,
                            const model::InferenceDelta& source) {
    return apply_sanitized_generation_delta(
        state, sink, state.session->utf8.on_delta(source));
}

nlohmann::json completed_stream_output(const ResponsesStreamState& state) {
    std::vector<std::pair<int, nlohmann::json>> indexed;
    if (state.reasoning_index) {
        indexed.push_back({*state.reasoning_index, {
            {"id", state.reasoning_id}, {"type", "reasoning"}, {"status", "completed"},
            {"summary", nlohmann::json::array()},
            {"content", nlohmann::json::array({{{"type", "reasoning_text"},
                                                  {"text", state.reasoning}}})},
        }});
    }
    if (state.message_index) {
        nlohmann::json content{
            {"type", "output_text"}, {"text", state.text},
            {"annotations", nlohmann::json::array()},
        };
        if (responses_request_wants_logprobs(state.request)) {
            content["logprobs"] = responses_logprobs(state.logprobs, true);
        }
        indexed.push_back({*state.message_index, {
            {"id", state.message_id}, {"type", "message"}, {"status", "completed"},
            {"role", "assistant"},
            {"content", nlohmann::json::array({std::move(content)})},
        }});
    }
    for (const auto& [_, tool] : state.tools) {
        indexed.push_back({tool.output_index, {
            {"id", tool.item_id}, {"type", "function_call"}, {"status", "completed"},
            {"call_id", tool.call_id}, {"name", tool.name}, {"arguments", tool.arguments},
        }});
    }
    std::sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    nlohmann::json output = nlohmann::json::array();
    for (auto& [_, item] : indexed) output.push_back(std::move(item));
    return output;
}

bool finish_response_stream(ResponsesStreamState& state, httplib::DataSink& sink) {
    if (state.completed) return true;
    if (state.reasoning_index) {
        if (!emit_response_event(state, sink, {
            {"type", "response.reasoning_text.done"}, {"item_id", state.reasoning_id},
            {"output_index", *state.reasoning_index}, {"content_index", 0},
            {"text", state.reasoning},
        })) return false;
        if (!emit_response_event(state, sink, {
            {"type", "response.content_part.done"}, {"item_id", state.reasoning_id},
            {"output_index", *state.reasoning_index}, {"content_index", 0},
            {"part", {{"type", "reasoning_text"}, {"text", state.reasoning}}},
        })) return false;
    }
    if (state.message_index) {
        if (!emit_response_event(state, sink, {
            {"type", "response.output_text.done"}, {"item_id", state.message_id},
            {"output_index", *state.message_index}, {"content_index", 0},
            {"text", state.text},
            {"logprobs", responses_logprobs(state.logprobs, false)},
        })) return false;
        nlohmann::json part{
            {"type", "output_text"}, {"text", state.text},
            {"annotations", nlohmann::json::array()},
        };
        if (responses_request_wants_logprobs(state.request)) {
            part["logprobs"] = responses_logprobs(state.logprobs, true);
        }
        if (!emit_response_event(state, sink, {
            {"type", "response.content_part.done"}, {"item_id", state.message_id},
            {"output_index", *state.message_index}, {"content_index", 0},
            {"part", std::move(part)},
        })) return false;
    }
    const auto output = completed_stream_output(state);
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto& item = output[index];
        if (item.value("type", "") == "function_call") {
            if (!emit_response_event(state, sink, {
                {"type", "response.function_call_arguments.done"},
                {"item_id", item["id"]}, {"output_index", index},
                {"name", item["name"]}, {"arguments", item["arguments"]},
            })) return false;
        }
        if (!emit_response_event(state, sink, {
            {"type", "response.output_item.done"}, {"output_index", index}, {"item", item},
        })) return false;
    }
    state.completed = true;
    return emit_response_event(state, sink, {
        {"type", "response.completed"},
        {"response", stream_response_object(state, "completed", output)},
    });
}

bool fail_response_stream(ResponsesStreamState& state, httplib::DataSink& sink,
                          const nlohmann::json& error) {
    if (state.completed) return true;
    state.completed = true;
    auto response = stream_response_object(state, "failed", nlohmann::json::array(), error);
    return emit_response_event(state, sink, {{"type", "response.failed"}, {"response", response}});
}


} // namespace

#include "embeddings_routes.ipp"

#include "responses_routes.ipp"

} // namespace inferdeck::gateway
