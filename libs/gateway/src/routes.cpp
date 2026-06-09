#include "gateway/routes.hpp"

#include "engine/token_sequence.hpp"
#include "gateway/streaming_sanitizer.hpp"
#include "foundation/logging.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace inferdeck::gateway {

using namespace inferdeck::foundation;

namespace {

std::string make_id() {
    static std::mutex mtx;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lock(mtx);
    return "chatcmpl-" + std::to_string(rng());
}

std::string sse_chunk(const std::string& id, const std::string& model,
                      const std::string& delta_json) {
    nlohmann::json chunk = {
        {"id", id},
        {"object", "chat.completion.chunk"},
        {"created", std::time(nullptr)},
        {"model", model},
        {"choices", nlohmann::json::array({
            {
                {"index", 0},
                {"delta", nlohmann::json::parse(delta_json)},
                {"finish_reason", nullptr},
            }
        })},
    };
    return "data: " + chunk.dump() + "\n\n";
}

std::string sse_chunk_json(const std::string& id, const std::string& model,
                           const nlohmann::json& delta) {
    return sse_chunk(id, model, delta.dump());
}

std::string sse_done(const std::string& id, const std::string& model,
                     const std::string& finish_reason = "stop") {
    nlohmann::json chunk = {
        {"id", id},
        {"object", "chat.completion.chunk"},
        {"created", std::time(nullptr)},
        {"model", model},
        {"choices", nlohmann::json::array({
            {
                {"index", 0},
                {"delta", nlohmann::json::object()},
                {"finish_reason", finish_reason},
            }
        })},
    };
    return "data: " + chunk.dump() + "\n\ndata: [DONE]\n\n";
}

nlohmann::json tool_call_json(const model::ToolCall& tc) {
    nlohmann::json out = {
        {"type", tc.type.empty() ? "function" : tc.type},
        {"function", {
            {"name", tc.function_name},
            {"arguments", tc.function_arguments},
        }},
    };
    if (!tc.id.empty()) out["id"] = tc.id;
    return out;
}

nlohmann::json tool_call_delta_json(const model::ToolCallDelta& tc) {
    nlohmann::json out = {{"index", tc.index}};
    if (!tc.id.empty()) {
        out["id"] = tc.id;
        out["type"] = tc.type.empty() ? "function" : tc.type;
    }
    if (!tc.function_name.empty() || !tc.function_arguments.empty()) {
        nlohmann::json fn = nlohmann::json::object();
        if (!tc.function_name.empty()) fn["name"] = tc.function_name;
        if (!tc.function_arguments.empty()) fn["arguments"] = tc.function_arguments;
        out["function"] = fn;
    }
    return out;
}

model::InferenceRequest make_inference_request(const nlohmann::json& body) {
    model::InferenceRequest ir;
    ir.openai_body_json = body.dump();
    ir.max_tokens = body.value("max_tokens", body.value("max_completion_tokens", 256));
    ir.temperature = static_cast<float>(body.value("temperature", 0.7));
    ir.top_p = static_cast<float>(body.value("top_p", 0.95));
    ir.top_k = body.value("top_k", 40);
    ir.repeat_penalty = static_cast<float>(body.value("repeat_penalty", 1.1));
    ir.repeat_last_n = body.value("repeat_last_n", 64);
    ir.seed = body.value("seed", -1);
    return ir;
}

nlohmann::json delta_json(const model::InferenceDelta& delta) {
    nlohmann::json out = nlohmann::json::object();
    if (!delta.reasoning_text.empty()) out["reasoning_content"] = delta.reasoning_text;
    if (!delta.content.empty()) out["content"] = delta.content;
    if (!delta.tool_calls.empty()) {
        out["tool_calls"] = nlohmann::json::array();
        for (const auto& tc : delta.tool_calls) {
            out["tool_calls"].push_back(tool_call_delta_json(tc));
        }
    }
    return out;
}

} // namespace

void write_json(httplib::Response& resp, int status,
                const nlohmann::json& body) {
    resp.status = status;
    resp.set_content(body.dump(), "application/json");
}

void write_error(httplib::Response& resp, int status, const std::string& code,
                 const std::string& message) {
    nlohmann::json body = {
        {"error", {{"code", code}, {"message", message}}},
    };
    write_json(resp, status, body);
}

std::string header_value(const httplib::Request& req, const std::string& name) {
    auto it = req.headers.find(name);
    if (it == req.headers.end()) return {};
    return it->second;
}

void handle_models(const httplib::Request& req, httplib::Response& resp,
                   const GatewayDeps& deps) {
    (void)req;
    nlohmann::json data = nlohmann::json::array();
    auto loaded = deps.coordinator.get_loaded_model();
    for (const auto& name : deps.coordinator.registry().list()) {
        const auto& info = deps.coordinator.registry().get_info(name);
        nlohmann::json entry = {
            {"id", name},
            {"object", "model"},
            {"created", std::time(nullptr)},
            {"owned_by", "inferdeck"},
            {"vram_required_mb", info.vram_required_mb},
            {"context_size", info.context_size},
            {"context_length", info.context_size},
            {"max_context_length", info.context_size},
            {"limit", {{"context", info.context_size}}},
            {"n_slots", info.n_slots},
            {"has_vision", info.has_vision},
            {"loaded", loaded && *loaded == name},
        };
        data.push_back(entry);
    }
    nlohmann::json body = {
        {"object", "list"},
        {"data", data},
    };
    write_json(resp, 200, body);
}

void handle_swap_to(const httplib::Request& req, httplib::Response& resp,
                    const GatewayDeps& deps, const std::string& model_name) {
    (void)req;
    if (!deps.coordinator.registry().has(model_name)) {
        write_error(resp, 404, "model_not_found",
                    "model not registered: " + model_name);
        return;
    }
    auto current = deps.coordinator.get_loaded_model();
    if (current && *current == model_name) {
        nlohmann::json body = {
            {"status", "ready"},
            {"model", model_name},
            {"message", "model already loaded"},
        };
        write_json(resp, 200, body);
        return;
    }
    // Synchronous swap for debugging
    LOG_INFO("swap_start", "model={}", model_name);
    auto swap_result = deps.coordinator.swap_to(model_name);
    LOG_INFO("swap_complete", "model={} success={}", model_name, swap_result.has_value());
    if (!swap_result) {
        LOG_ERROR("swap_failed", "model={} error={}", model_name, swap_result.error().message);
        write_error(resp, 500, "swap_failed", swap_result.error().message);
        return;
    }
    nlohmann::json body = {
        {"status", "ready"},
        {"model", model_name},
        {"message", "model loaded successfully"},
    };
    write_json(resp, 200, body);
}

void handle_swap_status(const httplib::Request& req, httplib::Response& resp,
                        const GatewayDeps& deps) {
    (void)req;
    auto current = deps.coordinator.get_loaded_model();
    nlohmann::json body = {
        {"loaded_model", current ? *current : ""},
        {"vram_usage_mb", deps.coordinator.get_vram_usage()},
        {"active_requests", deps.coordinator.active_request_count()},
    };
    write_json(resp, 200, body);
}

void handle_chat_completions(const httplib::Request& req, httplib::Response& resp,
                             const GatewayDeps& deps) {
    std::cerr << "DEBUG handle_chat_completions: start" << std::endl;
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        write_error(resp, 400, "invalid_json", e.what());
        return;
    }
    if (!body.contains("model") || !body["model"].is_string()) {
        write_error(resp, 400, "missing_model", "request body must include 'model'");
        return;
    }
    std::string model_name = body["model"].get<std::string>();
    if (model_name.size() > 7 && model_name.compare(model_name.size() - 7, 7, ":latest") == 0) {
        model_name.resize(model_name.size() - 7);
    }

    if (!deps.coordinator.registry().has(model_name)) {
        write_error(resp, 404, "model_not_found",
                    "model not registered: " + model_name);
        return;
    }
    if (!deps.coordinator.is_loaded(model_name)) {
        if (deps.auto_swap) {
            LOG_INFO("auto_swap_begin", "requested={}", model_name);
            auto swap_result = deps.coordinator.swap_to(model_name);
            if (!swap_result) {
                LOG_ERROR("auto_swap_failed", "model={} error={}", model_name, swap_result.error().message);
                write_error(resp, 500, "swap_failed", swap_result.error().message);
                return;
            }
            LOG_INFO("auto_swap_complete", "model={}", model_name);
        } else {
            resp.status = 503;
            resp.set_header("Retry-After", deps.default_swap_timeout_s);
            write_error(resp, 503, "model_not_loaded",
                        "model not loaded; POST /v1/swap/to/" + model_name +
                        " then retry");
            return;
        }
    }

    bool stream = body.value("stream", false);

    int slot_id = -1;
    {
        model::AcquireSlotOptions opts;
        opts.timeout = std::chrono::milliseconds{30000};
        opts.block = true;
        auto sr = deps.coordinator.acquire_slot(model_name, opts);
        if (!sr) {
            int status = 503;
            std::string code = "no_slots";
            if (sr.error().code == foundation::ErrorCode::Timeout) {
                status = 503;
                code = "slot_timeout";
            } else if (sr.error().code == foundation::ErrorCode::Cancelled) {
                status = 503;
                code = "cancelled";
            } else if (sr.error().code == foundation::ErrorCode::NotFound) {
                status = 404;
                code = "model_not_loaded";
            }
            resp.set_header("Retry-After", "1");
            write_error(resp, status, code, sr.error().message);
            return;
        }
        slot_id = *sr;
    }

    const std::string id = make_id();
    const std::string stream_model = model_name;

    std::function<void()> release_slot = [slot_id, &deps, model_name]() noexcept {
        if (slot_id >= 0) {
            (void)deps.coordinator.release_slot(model_name, slot_id);
        }
    };
    struct ReleaseGuard {
        std::function<void()>* fn;
        bool armed{true};
        void disarm() { armed = false; }
        ~ReleaseGuard() { if (armed && fn) (*fn)(); }
    } guard{&release_slot};

    if (!stream) {
        model::InferenceRequest ir = make_inference_request(body);

        model::InferenceResult pr;
        try {
            auto pres = deps.coordinator.predict(model_name, slot_id, ir);
            if (!pres) {
                write_error(resp, 500, "inference_error", pres.error().message);
                return;
            }
            pr = std::move(*pres);
        } catch (const std::exception& e) {
            LOG_ERROR("predict_exception", "what={}", e.what());
            write_error(resp, 500, "inference_exception", e.what());
            return;
        } catch (...) {
            LOG_ERROR("predict_unknown_exception", "");
            write_error(resp, 500, "inference_exception", "unknown exception");
            return;
        }

        nlohmann::json message = {
            {"role", "assistant"},
            {"content", pr.text},
        };
        if (!pr.reasoning_text.empty()) {
            message["reasoning_content"] = pr.reasoning_text;
        }
        if (!pr.tool_calls.empty()) {
            message["tool_calls"] = nlohmann::json::array();
            for (const auto& tc : pr.tool_calls) {
                message["tool_calls"].push_back(tool_call_json(tc));
            }
        }
        const std::string finish_reason = !pr.tool_calls.empty() ? "tool_calls" : pr.finish_reason;
        nlohmann::json resp_body = {
            {"id", id},
            {"object", "chat.completion"},
            {"created", std::time(nullptr)},
            {"model", stream_model},
            {"choices", nlohmann::json::array({
                {
                    {"index", 0},
                    {"message", message},
                    {"finish_reason", finish_reason},
                }
            })},
            {"usage", {
                {"prompt_tokens", pr.prompt_tokens},
                {"completion_tokens", pr.completion_tokens},
                {"total_tokens", pr.prompt_tokens + pr.completion_tokens},
            }},
        };
        write_json(resp, 200, resp_body);
        return;
    }

    resp.set_header("Content-Type", "text/event-stream");
    resp.set_header("Cache-Control", "no-cache");
    resp.set_header("Connection", "keep-alive");

    model::InferenceRequest ir = make_inference_request(body);

    struct StreamState {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<model::InferenceDelta> delta_queue;
        bool inference_done{false};
        bool inference_error{false};
        std::string error_msg;
        std::shared_ptr<model::InferenceResult> final_result;
        std::atomic<bool> aborted{false};
        std::thread inference_thread;
        int slot_id{-1};
        std::string model_name;
        model::BackendCoordinator* coordinator{nullptr};
    };

    auto state = std::make_shared<StreamState>();
    state->slot_id = slot_id;
    state->model_name = model_name;
    state->coordinator = &deps.coordinator;

    guard.disarm();

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
                });
            {
                std::lock_guard<std::mutex> lk(state->mtx);
                if (result) {
                    state->final_result = std::make_shared<model::InferenceResult>(std::move(*result));
                } else {
                    state->inference_error = true;
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

    auto last_heartbeat = std::chrono::steady_clock::now();
    const auto heartbeat_interval = std::chrono::seconds{10};

    resp.set_chunked_content_provider(
        "text/event-stream",
        [id, stream_model, state, last_heartbeat = std::move(last_heartbeat),
         heartbeat_interval = std::move(heartbeat_interval)](
            std::size_t, httplib::DataSink& sink) mutable {
            std::unique_lock<std::mutex> lk(state->mtx);

            auto now = std::chrono::steady_clock::now();
            if (now - last_heartbeat >= heartbeat_interval) {
                last_heartbeat = now;
                sink.write(": \n\n", 5);
            }

            state->cv.wait(lk, [&] {
                return !state->delta_queue.empty() || state->inference_done;
            });

            if (!state->delta_queue.empty()) {
                std::deque<model::InferenceDelta> deltas;
                while (!state->delta_queue.empty()) {
                    deltas.push_back(std::move(state->delta_queue.front()));
                    state->delta_queue.pop_front();
                }
                lk.unlock();

                if (!sink.is_writable()) {
                    state->aborted.store(true);
                    state->cv.notify_all();
                    if (state->inference_thread.joinable()) {
                        state->inference_thread.join();
                    }
                    state->coordinator->release_slot(state->model_name, state->slot_id);
                    return false;
                }

                for (const auto& delta : deltas) {
                    auto json_delta = delta_json(delta);
                    if (json_delta.empty()) continue;
                    std::string out = sse_chunk_json(id, stream_model, json_delta);
                    if (!sink.write(out.data(), out.size())) {
                        state->aborted.store(true);
                        state->cv.notify_all();
                        if (state->inference_thread.joinable()) {
                            state->inference_thread.join();
                        }
                        state->coordinator->release_slot(state->model_name, state->slot_id);
                        return false;
                    }
                }
                return true;
            }

            if (state->inference_error) {
                std::string err = "data: " + nlohmann::json{{"error", {{"message", state->error_msg}}}}.dump() + "\n\n";
                sink.write(err.data(), err.size());
            } else {
                const bool has_tool_calls = state->final_result && !state->final_result->tool_calls.empty();
                const std::string finish_reason = has_tool_calls ? "tool_calls" :
                    (state->final_result ? state->final_result->finish_reason : "stop");
                std::string done = sse_done(id, stream_model, finish_reason);
                sink.write(done.data(), done.size());
            }
            sink.done();

            if (state->inference_thread.joinable()) {
                state->inference_thread.join();
            }
            state->coordinator->release_slot(state->model_name, state->slot_id);
            return false;
        });
}

} // namespace inferdeck::gateway
