#include "gateway/anthropic_routes.hpp"

#include "foundation/logging.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace inferdeck::gateway {

using namespace inferdeck::foundation;

namespace {

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

std::string sse_event(const std::string& name, const nlohmann::json& data) {
    return "event: " + name + "\ndata: " + data.dump() + "\n\n";
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
    ir.max_tokens = openai_body.value("max_tokens", -1);
    ir.temperature = static_cast<float>(openai_body.value("temperature", 0.7));
    ir.top_p = static_cast<float>(openai_body.value("top_p", 0.95));
    ir.top_k = openai_body.value("top_k", 40);
    return ir;
}

struct ErrorClass {
    int status;
    std::string type;
};

ErrorClass classify_error(foundation::ErrorCode code) {
    const bool invalid = code == foundation::ErrorCode::InvalidArgument ||
                         code == foundation::ErrorCode::ParseError;
    if (invalid) return {400, "invalid_request_error"};
    return {500, "api_error"};
}

} // namespace

std::string resolve_anthropic_model(const GatewayDeps& deps, const std::string& requested) {
    auto alias = deps.anthropic_model_aliases.find(requested);
    if (alias != deps.anthropic_model_aliases.end()) return alias->second;
    if (deps.coordinator.registry().has(requested)) return requested;
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
            auto tool_calls = nlohmann::json::array();
            for (const auto& block : content) {
                const std::string type = block.value("type", "");
                if (type == "text") {
                    text += block.value("text", "");
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
                // thinking / redacted_thinking blocks are dropped
            }
            m["content"] = text;
            if (!tool_calls.empty()) m["tool_calls"] = tool_calls;
            messages.push_back(std::move(m));
            continue;
        }

        // user message: tool_result blocks become OpenAI tool messages,
        // text/image blocks become a user message.
        std::string text;
        auto image_parts = nlohmann::json::array();
        for (const auto& block : content) {
            const std::string type = block.value("type", "");
            if (type == "tool_result") {
                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", block.value("tool_use_id", "")},
                    {"content", flatten_text(block.contains("content") ? block["content"]
                                                                       : nlohmann::json(""))},
                });
            } else if (type == "text") {
                if (!text.empty()) text += "\n";
                text += block.value("text", "");
            } else if (type == "image") {
                const auto& src = block.value("source", nlohmann::json::object());
                if (src.value("type", "") == "base64") {
                    image_parts.push_back({
                        {"type", "image_url"},
                        {"image_url", {{"url", "data:" + src.value("media_type", "image/png") +
                                               ";base64," + src.value("data", "")}}},
                    });
                } else if (src.value("type", "") == "url") {
                    image_parts.push_back({
                        {"type", "image_url"},
                        {"image_url", {{"url", src.value("url", "")}}},
                    });
                }
            }
        }
        if (!image_parts.empty()) {
            auto parts = nlohmann::json::array();
            if (!text.empty()) parts.push_back({{"type", "text"}, {"text", text}});
            for (auto& p : image_parts) parts.push_back(std::move(p));
            messages.push_back({{"role", "user"}, {"content", parts}});
        } else if (!text.empty()) {
            messages.push_back({{"role", "user"}, {"content", text}});
        }
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
    (void)deps;
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        write_anthropic_error(resp, 400, "invalid_request_error", e.what());
        return;
    }
    // No tokenizer API is exposed at the gateway layer; estimate ~4 chars/token
    // over the serialized conversation. Claude Code only needs a rough figure
    // for context accounting.
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
    } catch (const std::exception& e) {
        write_anthropic_error(resp, 400, "invalid_request_error",
                              std::string("invalid JSON: ") + e.what());
        return;
    }
    if (!body.contains("messages") || !body["messages"].is_array()) {
        write_anthropic_error(resp, 400, "invalid_request_error",
                              "request body must include 'messages' array");
        return;
    }
    if (!body.contains("max_tokens") || !body["max_tokens"].is_number()) {
        write_anthropic_error(resp, 400, "invalid_request_error",
                              "request body must include 'max_tokens'");
        return;
    }

    const std::string requested_model = body.value("model", "");
    const std::string model_name = resolve_anthropic_model(deps, requested_model);
    if (model_name.empty()) {
        write_anthropic_error(resp, 404, "not_found_error",
                              "no model available for: " + requested_model);
        return;
    }

    nlohmann::json openai_body;
    try {
        openai_body = anthropic_to_openai(body, model_name);
    } catch (const std::exception& e) {
        write_anthropic_error(resp, 400, "invalid_request_error", e.what());
        return;
    }

    {
        auto loaded = ensure_model_loaded(deps, model_name);
        if (!loaded.ok) {
            if (loaded.status == 503) {
                resp.set_header("Retry-After", deps.default_swap_timeout_s);
            }
            const char* type =
                loaded.status == 404 ? "not_found_error" : "overloaded_error";
            write_anthropic_error(resp, loaded.status, type, loaded.message);
            return;
        }
    }

    const bool stream = body.value("stream", false);

    int slot_id = -1;
    {
        model::AcquireSlotOptions opts;
        opts.timeout = std::chrono::milliseconds{30000};
        opts.block = true;
        auto sr = deps.coordinator.acquire_slot(model_name, opts);
        if (!sr) {
            resp.set_header("Retry-After", "1");
            write_anthropic_error(resp, 503, "overloaded_error", sr.error().message);
            return;
        }
        slot_id = *sr;
    }

    const std::string id = make_msg_id();
    model::InferenceRequest ir = make_inference_request_from(openai_body);

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
                record_request(deps, model_name, failed, ec.status, slot_id);
                write_anthropic_error(resp, ec.status, ec.type, pres.error().message);
                return;
            }
            pr = std::move(*pres);
        }
        record_request(deps, model_name, pr, 200, slot_id);

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
        bool inference_done{false};
        bool inference_error{false};
        foundation::ErrorCode error_code{foundation::ErrorCode::Internal};
        std::string error_msg;
        std::shared_ptr<model::InferenceResult> final_result;
        std::atomic<bool> aborted{false};
        std::thread inference_thread;
        int slot_id{-1};
        std::string model_name;
        model::BackendCoordinator* coordinator{nullptr};
        observability::Metrics* metrics{nullptr};
        observability::StatsDb* stats_db{nullptr};
        foundation::EventBus* events{nullptr};
        std::atomic<bool> cleanup_done{false};

        // Serialization state for the chunked provider (single-threaded use).
        AnthropicBlockState blocks;
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
                record_request(metrics, stats_db, events, model_name, *result, 200, slot_id);
            } else {
                model::InferenceResult failed;
                record_request(metrics, stats_db, events, model_name, failed,
                               aborted_stream ? 499 : 500, slot_id);
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
    state->coordinator = &deps.coordinator;
    state->metrics = deps.metrics;
    state->stats_db = deps.stats_db;
    state->events = deps.events;

    state->inference_thread = std::thread([state, ir]() {
        try {
            auto result = state->coordinator->predict_stream(
                state->model_name, state->slot_id, ir,
                [state](const model::InferenceDelta& delta) -> bool {
                    if (state->aborted.load()) return false;
                    {
                        std::lock_guard<std::mutex> lk(state->mtx);
                        state->delta_queue.push_back(delta);
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
                        deltas.push_back(std::move(state->delta_queue.front()));
                        state->delta_queue.pop_front();
                    }
                    lk.unlock();

                    for (const auto& delta : deltas) {
                        std::string out = state->blocks.from_delta(delta);
                        if (!delta.tool_calls.empty()) state->streamed_any_tool = true;
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

                if (inference_error) {
                    LOG_ERROR("anthropic_inference_failed", "model={} slot_id={} error={}",
                              state->model_name, state->slot_id, error_msg);
                    nlohmann::json err = {
                        {"type", "error"},
                        {"error", {{"type", "api_error"}, {"message", error_msg}}},
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
