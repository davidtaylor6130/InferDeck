#include "gateway/routes.hpp"

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

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

void record_request(observability::Metrics* metrics,
                    observability::StatsDb* stats_db,
                    foundation::EventBus* events,
                    const std::string& model_name,
                    const model::InferenceResult& result,
                    int status_code,
                    int slot_id) {
    observability::RequestRecord rec;
    rec.timestamp_unix_ms = now_ms();
    rec.model = model_name;
    rec.prompt_tokens = result.prompt_tokens;
    rec.completion_tokens = result.completion_tokens;
    rec.duration_ms = result.duration_ms;
    rec.tokens_per_second = result.tokens_per_second;
    rec.status_code = status_code;
    rec.slot_id = slot_id;
    if (metrics) metrics->record_request(rec);
    LOG_INFO("request_recorded",
             "model={} status={} slot_id={} prompt_tokens={} cached_prompt_tokens={} completion_tokens={} duration_ms={} tps={}",
             model_name,
             status_code,
             slot_id,
             result.prompt_tokens,
             result.cached_prompt_tokens,
             result.completion_tokens,
             result.duration_ms,
             result.tokens_per_second);
    if (stats_db) {
        stats_db->record_request({
            rec.timestamp_unix_ms,
            rec.model,
            rec.prompt_tokens,
            rec.completion_tokens,
            rec.duration_ms,
            rec.tokens_per_second,
            rec.status_code,
            rec.slot_id
        });
    }
    if (events) {
        events->publish("request", nlohmann::json{
            {"timestampUnixMs", rec.timestamp_unix_ms},
            {"model", model_name},
            {"promptTokens", result.prompt_tokens},
            {"completionTokens", result.completion_tokens},
            {"durationMs", result.duration_ms},
            {"tokensPerSecond", result.tokens_per_second},
            {"status", status_code},
        }.dump());
    }
}

void record_request(const GatewayDeps& deps,
                    const std::string& model_name,
                    const model::InferenceResult& result,
                    int status_code,
                    int slot_id) {
    record_request(deps.metrics, deps.stats_db, deps.events,
                   model_name, result, status_code, slot_id);
}

namespace {

void record_swap(const GatewayDeps& deps,
                 const std::string& from_model,
                 const std::string& to_model,
                 double duration_ms,
                 bool success,
                 const std::string& error) {
    observability::SwapRecord rec;
    rec.timestamp_unix_ms = now_ms();
    rec.from_model = from_model;
    rec.to_model = to_model;
    rec.duration_ms = duration_ms;
    rec.success = success;
    rec.error = error;
    if (deps.metrics) deps.metrics->record_swap(rec);
    if (deps.stats_db) {
        deps.stats_db->record_swap({
            rec.timestamp_unix_ms,
            rec.from_model,
            rec.to_model,
            rec.duration_ms,
            rec.success,
            rec.error
        });
    }
}

void publish_model_event(const GatewayDeps& deps, const std::string& state,
                         const std::string& from, const std::string& to,
                         double duration_ms, const std::string& error) {
    if (!deps.events) return;
    deps.events->publish("model", nlohmann::json{
        {"state", state},
        {"from", from},
        {"to", to},
        {"durationMs", duration_ms},
        {"error", error},
        {"timestampUnixMs", now_ms()},
    }.dump());
}

foundation::Result<void> perform_swap(const GatewayDeps& deps,
                                      const std::string& from,
                                      const std::string& target) {
    LOG_INFO("swap_start", "from={} to={}", from, target);
    const auto start = std::chrono::steady_clock::now();
    foundation::Result<void> result;
    try {
        result = deps.coordinator.swap_to_cancellable(target);
    } catch (const std::exception& e) {
        result = foundation::Err(foundation::ErrorCode::Internal, e.what());
    } catch (...) {
        result = foundation::Err(foundation::ErrorCode::Internal, "swap threw unknown exception");
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    const std::string error = result ? std::string{} : result.error().message;
    const bool cancelled = !result && result.error().code == foundation::ErrorCode::Cancelled;
    LOG_INFO("swap_complete", "to={} success={} duration_ms={} error={}",
             target, result.has_value(), elapsed, error);
    record_swap(deps, from, target, elapsed, result.has_value(), error);
    publish_model_event(deps, result ? "ready" : (cancelled ? "cancelled" : "failed"),
                        from, target, elapsed, error);
    if (deps.swap_tracker) deps.swap_tracker->end(result.has_value(), error);
    return result;
}

bool begin_tracked_swap(const GatewayDeps& deps, const std::string& from,
                        const std::string& target) {
    if (deps.swap_tracker && !deps.swap_tracker->begin(from, target, now_ms())) {
        return false;
    }
    publish_model_event(deps, "swapping", from, target, 0.0, "");
    return true;
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
    ir.max_tokens = body.value("max_tokens", body.value("max_completion_tokens", -1));
    // Only carry sampler params the client explicitly set; unset ones fall back
    // to the server-side SamplingConfig defaults downstream (issue #42).
    if (body.contains("temperature") && !body["temperature"].is_null())
        ir.temperature = body["temperature"].get<float>();
    if (body.contains("top_p") && !body["top_p"].is_null())
        ir.top_p = body["top_p"].get<float>();
    if (body.contains("top_k") && !body["top_k"].is_null())
        ir.top_k = body["top_k"].get<int>();
    if (body.contains("repeat_penalty") && !body["repeat_penalty"].is_null())
        ir.repeat_penalty = body["repeat_penalty"].get<float>();
    if (body.contains("repeat_last_n") && !body["repeat_last_n"].is_null())
        ir.repeat_last_n = body["repeat_last_n"].get<int>();
    ir.seed = body.value("seed", -1);
    return ir;
}

struct ErrorClass {
    int status;
    std::string type;
    std::string code;
};

ErrorClass classify_inference_error(foundation::ErrorCode code, const std::string& message) {
    const bool invalid = code == foundation::ErrorCode::InvalidArgument ||
                         code == foundation::ErrorCode::ParseError;
    if (!invalid) return {500, "server_error", "inference_error"};
    if (message.find("maximum context length") != std::string::npos) {
        return {400, "invalid_request_error", "context_length_exceeded"};
    }
    return {400, "invalid_request_error", "invalid_request_error"};
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

SwapStartResult start_swap_async(const GatewayDeps& deps, const std::string& model_name) {
    if (!deps.coordinator.registry().has(model_name)) {
        return {404, {{"error", {{"code", "model_not_found"},
                                 {"message", "model not registered: " + model_name}}}}};
    }
    auto current = deps.coordinator.get_loaded_model();
    if (current && *current == model_name) {
        return {200, {{"status", "ready"},
                      {"model", model_name},
                      {"message", "model already loaded"}}};
    }
    if (!begin_tracked_swap(deps, current.value_or(""), model_name)) {
        const auto snap = deps.swap_tracker ? deps.swap_tracker->snapshot() : SwapSnapshot{};
        return {409, {{"error", {{"code", "swap_in_progress"},
                                 {"message", "a swap to " + snap.target + " is already in progress"}}}}};
    }
    GatewayDeps deps_copy = deps;
    const std::string from = current.value_or("");
    std::thread([deps_copy, from, model_name]() {
        (void)perform_swap(deps_copy, from, model_name);
    }).detach();
    return {202, {{"status", "swapping"},
                  {"model", model_name},
                  {"from", from}}};
}

EnsureLoadedResult ensure_model_loaded(const GatewayDeps& deps,
                                       const std::string& model_name) {
    if (deps.coordinator.is_loaded(model_name)) {
        return {true, 200, "", ""};
    }
    if (!deps.auto_swap) {
        return {false, 503, "model_not_loaded",
                "model not loaded; POST /v1/swap/to/" + model_name + " then retry"};
    }

    LOG_INFO("auto_swap_begin", "requested={}", model_name);
    // Wait out any in-progress swap rather than failing fast with 503. A cold-start
    // race (e.g. a client firing a title + main request at the same time) resolves
    // when the first swap finishes and the target becomes loaded -- no redundant
    // swap, no 503.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{5};
    while (std::chrono::steady_clock::now() < deadline) {
        if (deps.coordinator.is_loaded(model_name)) {
            return {true, 200, "", ""};
        }

        auto started = start_swap_async(deps, model_name);
        if (started.status == 404) {
            return {false, 404, "model_not_found",
                    "model not registered: " + model_name};
        }
        // status 200 (already loaded), 202 (we started the swap), or 409 (another
        // swap already in progress): in every case wait for the model to load.
        while (std::chrono::steady_clock::now() < deadline) {
            if (deps.coordinator.is_loaded(model_name)) {
                return {true, 200, "", ""};
            }
            const auto snap =
                deps.swap_tracker ? deps.swap_tracker->snapshot() : SwapSnapshot{};
            if (!snap.swapping && !deps.coordinator.swap_in_progress()) {
                // The in-progress swap settled. If it targeted our model and failed,
                // surface that; otherwise re-evaluate and start our own swap.
                if (snap.target == model_name && !snap.last_error.empty()) {
                    return {false, 503, "swap_failed",
                            "model load failed: " + snap.last_error};
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }
    return {false, 503, "swap_in_progress", "model load timed out: " + model_name};
}

void handle_swap_to(const httplib::Request& req, httplib::Response& resp,
                    const GatewayDeps& deps, const std::string& model_name) {
    (void)req;
    auto started = start_swap_async(deps, model_name);
    write_json(resp, started.status, started.body);
}

void handle_swap_cancel(const httplib::Request& req, httplib::Response& resp,
                        const GatewayDeps& deps) {
    (void)req;
    const auto snap = deps.swap_tracker ? deps.swap_tracker->snapshot() : SwapSnapshot{};
    if (!snap.swapping && !deps.coordinator.swap_in_progress()) {
        write_json(resp, 200, {{"status", "idle"}, {"message", "no swap in progress"}});
        return;
    }
    deps.coordinator.request_swap_cancel();
    LOG_INFO("swap_cancel_requested", "target={}", snap.target);
    write_json(resp, 202, {{"status", "cancelling"}, {"target", snap.target}});
}

void handle_swap_status(const httplib::Request& req, httplib::Response& resp,
                        const GatewayDeps& deps) {
    (void)req;
    auto current = deps.coordinator.get_loaded_model();
    const auto snap = deps.swap_tracker ? deps.swap_tracker->snapshot() : SwapSnapshot{};
    nlohmann::json body = {
        {"loaded_model", current ? *current : ""},
        {"vram_usage_mb", deps.coordinator.get_vram_usage()},
        {"active_requests", deps.coordinator.active_request_count()},
        {"swapping", snap.swapping},
        {"target", snap.target},
        {"from", snap.from},
        {"started_unix_ms", snap.started_unix_ms},
        {"last_error", snap.last_error},
    };
    write_json(resp, 200, body);
}

void handle_chat_completions(const httplib::Request& req, httplib::Response& resp,
                             const GatewayDeps& deps) {
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
    {
        auto loaded = ensure_model_loaded(deps, model_name);
        if (!loaded.ok) {
            if (loaded.status == 503) {
                resp.set_header("Retry-After", deps.default_swap_timeout_s);
            }
            write_error(resp, loaded.status, loaded.code, loaded.message);
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
            model::InferenceResult failed;
            record_request(deps, model_name, failed, status, -1);
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
                const auto ec = classify_inference_error(pres.error().code, pres.error().message);
                LOG_ERROR("inference_failed",
                          "model={} slot_id={} status={} code={} error={}",
                          model_name, slot_id, ec.status, ec.code, pres.error().message);
                model::InferenceResult failed;
                record_request(deps, model_name, failed, ec.status, slot_id);
                write_error(resp, ec.status, ec.code, pres.error().message);
                return;
            }
            pr = std::move(*pres);
        } catch (const std::exception& e) {
            LOG_ERROR("predict_exception", "what={}", e.what());
            model::InferenceResult failed;
            record_request(deps, model_name, failed, 500, slot_id);
            write_error(resp, 500, "inference_exception", e.what());
            return;
        } catch (...) {
            LOG_ERROR("predict_unknown_exception", "");
            model::InferenceResult failed;
            record_request(deps, model_name, failed, 500, slot_id);
            write_error(resp, 500, "inference_exception", "unknown exception");
            return;
        }
        record_request(deps, model_name, pr, 200, slot_id);

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
                {"prompt_tokens_details", {
                    {"cached_tokens", pr.cached_prompt_tokens},
                }},
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

        void finish_once(bool aborted_stream, int fallback_status, const std::string& reason) {
            bool expected = false;
            if (!cleanup_done.compare_exchange_strong(expected, true)) return;
            if (aborted_stream) {
                aborted.store(true);
            }
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

            int status = fallback_status;
            if (result && !aborted_stream && !error) {
                status = 200;
                record_request(metrics, stats_db, events, model_name, *result, status, slot_id);
            } else {
                model::InferenceResult failed;
                status = aborted_stream ? 499
                       : (error && fallback_status < 400 ? 500 : fallback_status);
                record_request(metrics, stats_db, events, model_name, failed, status, slot_id);
            }

            LOG_INFO("stream_recorded", "model={} slot_id={} status={} reason={}",
                     model_name, slot_id, status, reason);
            if (coordinator) {
                auto released = coordinator->release_slot(model_name, slot_id);
                if (!released) {
                    LOG_WARN("stream_cleanup_release_failed", "model={} slot_id={} reason={}",
                             model_name, slot_id, released.error().message);
                }
            }
            LOG_INFO("stream_cleanup", "model={} slot_id={} aborted={} reason={}",
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
            },
            &state->aborted);
            {
                std::lock_guard<std::mutex> lk(state->mtx);
                if (result) {
                    state->final_result = std::make_shared<model::InferenceResult>(std::move(*result));
                } else {
                    state->inference_error = true;
                    state->error_code = result.error().code;
                    state->error_msg = result.error().message;
                }
                state->inference_done = true;
            }
            state->cv.notify_all();
        } catch (const std::exception& e) {
            LOG_ERROR("inference_thread_exception", "model={} slot_id={} what={}",
                      state->model_name, state->slot_id, e.what());
            std::lock_guard<std::mutex> lk(state->mtx);
            state->inference_error = true;
            state->error_msg = e.what();
            state->inference_done = true;
            state->cv.notify_all();
        } catch (...) {
            LOG_ERROR("inference_thread_exception", "model={} slot_id={} what=unknown",
                      state->model_name, state->slot_id);
            std::lock_guard<std::mutex> lk(state->mtx);
            state->inference_error = true;
            state->error_msg = "unknown exception";
            state->inference_done = true;
            state->cv.notify_all();
        }
    });

    resp.set_chunked_content_provider(
        "text/event-stream",
        [id, stream_model, state](
            std::size_t, httplib::DataSink& sink) mutable {
            try {
            std::unique_lock<std::mutex> lk(state->mtx);

            while (!state->cv.wait_for(lk, std::chrono::seconds{2}, [&] {
                return !state->delta_queue.empty() || state->inference_done || state->aborted.load();
            })) {
                lk.unlock();
                if (!sink.write(": \n\n", 4)) {
                    LOG_WARN("stream_abort", "model={} slot_id={} reason=heartbeat_write_failed",
                             state->model_name, state->slot_id);
                    state->finish_once(true, 499, "heartbeat_write_failed");
                    return false;
                }
                lk.lock();
            }

            if (state->aborted.load() && !state->inference_done && state->delta_queue.empty()) {
                lk.unlock();
                LOG_WARN("stream_abort", "model={} slot_id={} reason=aborted",
                         state->model_name, state->slot_id);
                state->finish_once(true, 499, "aborted");
                return false;
            }

            if (!state->delta_queue.empty()) {
                std::deque<model::InferenceDelta> deltas;
                while (!state->delta_queue.empty()) {
                    deltas.push_back(std::move(state->delta_queue.front()));
                    state->delta_queue.pop_front();
                }
                lk.unlock();

                if (!sink.is_writable()) {
                    LOG_WARN("stream_abort", "model={} slot_id={} reason=sink_not_writable",
                             state->model_name, state->slot_id);
                    state->finish_once(true, 499, "sink_not_writable");
                    return false;
                }

                for (const auto& delta : deltas) {
                    auto json_delta = delta_json(delta);
                    if (json_delta.empty()) continue;
                    std::string out = sse_chunk_json(id, stream_model, json_delta);
                    if (!sink.write(out.data(), out.size())) {
                        LOG_WARN("stream_abort", "model={} slot_id={} reason=chunk_write_failed",
                                 state->model_name, state->slot_id);
                        state->finish_once(true, 499, "chunk_write_failed");
                        return false;
                    }
                }
                return true;
            }

            const bool inference_error = state->inference_error;
            const auto error_code = state->error_code;
            const std::string error_msg = state->error_msg;
            const auto final_result = state->final_result;
            lk.unlock();

            if (inference_error) {
                const auto ec = classify_inference_error(error_code, error_msg);
                LOG_ERROR("inference_failed",
                          "model={} slot_id={} status={} code={} error={}",
                          state->model_name, state->slot_id, ec.status, ec.code, error_msg);
                std::string err = "data: " + nlohmann::json{{"error", {
                    {"message", error_msg},
                    {"type", ec.type},
                    {"code", ec.code},
                }}}.dump() + "\n\n";
                if (!sink.write(err.data(), err.size())) {
                    LOG_WARN("stream_abort", "model={} slot_id={} reason=error_write_failed",
                             state->model_name, state->slot_id);
                    state->finish_once(true, 499, "error_write_failed");
                    return false;
                }
                state->finish_once(false, ec.status, "inference_error");
            } else {
                const bool has_tool_calls = final_result && !final_result->tool_calls.empty();
                const std::string finish_reason = has_tool_calls ? "tool_calls" :
                    (final_result ? final_result->finish_reason : "stop");
                std::string done = sse_done(id, stream_model, finish_reason);
                if (!sink.write(done.data(), done.size())) {
                    LOG_WARN("stream_abort", "model={} slot_id={} reason=done_write_failed",
                             state->model_name, state->slot_id);
                    state->finish_once(true, 499, "done_write_failed");
                    return false;
                }
                state->finish_once(false, 200, "completed");
            }
            sink.done();
            return false;
            } catch (const std::exception& e) {
                LOG_ERROR("stream_provider_exception", "model={} slot_id={} what={}",
                          state->model_name, state->slot_id, e.what());
                state->finish_once(true, 500, "provider_exception");
                return false;
            } catch (...) {
                LOG_ERROR("stream_provider_unknown_exception", "model={} slot_id={}",
                          state->model_name, state->slot_id);
                state->finish_once(true, 500, "provider_unknown_exception");
                return false;
            }
        },
        [state](bool success) {
            if (!success) {
                LOG_WARN("stream_abort", "model={} slot_id={} reason=resource_releaser",
                         state->model_name, state->slot_id);
                state->finish_once(true, 499, "resource_releaser");
            } else {
                state->finish_once(false, 200, "resource_releaser_success");
            }
        });
}

} // namespace inferdeck::gateway
