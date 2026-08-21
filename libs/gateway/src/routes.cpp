#include "gateway/routes.hpp"

#include "gateway/openai_adapter.hpp"
#include "gateway/openai_error.hpp"
#include "gateway/generation_session.hpp"
#include "gateway/streaming_sanitizer.hpp"
#include "foundation/logging.hpp"

#include <algorithm>
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
#include <unordered_map>

namespace inferdeck::gateway {

using namespace inferdeck::foundation;

namespace {

constexpr std::size_t max_pending_stream_deltas = 128;
constexpr std::size_t max_pending_stream_bytes = 1024 * 1024;

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
                    int slot_id,
                    double input_audio_seconds,
                    std::int64_t input_characters,
                    const std::string& resolved_model_name) {
    observability::RequestRecord rec;
    rec.timestamp_unix_ms = now_ms();
    rec.model = model_name;
    rec.prompt_tokens = result.prompt_tokens;
    rec.completion_tokens = result.completion_tokens;
    rec.duration_ms = result.duration_ms;
    rec.tokens_per_second = result.tokens_per_second;
    rec.status_code = status_code;
    rec.slot_id = slot_id;
    const double prompt_duration_ms = result.prompt_duration_ms > 0.0f
        ? result.prompt_duration_ms
        : std::max(0.0, static_cast<double>(
              result.duration_ms - result.generation_duration_ms));
    const int evaluated_prompt_tokens =
        std::max(0, result.prompt_tokens - result.cached_prompt_tokens);
    if (metrics) metrics->record_request(rec);
    LOG_INFO("request_recorded",
             "model={} status={} slot_id={} prompt_tokens={} cached_prompt_tokens={} completion_tokens={} duration_ms={} generation_duration_ms={} tps={}",
             model_name,
             status_code,
             slot_id,
             result.prompt_tokens,
             result.cached_prompt_tokens,
             result.completion_tokens,
             result.duration_ms,
             result.generation_duration_ms,
             result.tokens_per_second);
    if (stats_db) {
        observability::RequestRow row;
        row.timestamp_unix_ms = rec.timestamp_unix_ms;
        row.model = rec.model;
        row.resolved_model = resolved_model_name.empty() ? rec.model : resolved_model_name;
        row.prompt_tokens = rec.prompt_tokens;
        row.cached_prompt_tokens = result.cached_prompt_tokens;
        row.completion_tokens = rec.completion_tokens;
        row.duration_ms = rec.duration_ms;
        row.generation_duration_ms = result.generation_duration_ms;
        row.prompt_duration_ms = prompt_duration_ms;
        row.tokens_per_second = rec.tokens_per_second;
        row.prompt_tokens_per_second = row.prompt_duration_ms > 0.0
            ? static_cast<double>(evaluated_prompt_tokens) / (row.prompt_duration_ms / 1000.0)
            : 0.0;
        row.status_code = rec.status_code;
        row.slot_id = rec.slot_id;
        row.input_audio_seconds = input_audio_seconds;
        row.input_characters = input_characters;
        stats_db->record_request(row);
    }
    if (events) {
        events->publish("request", nlohmann::json{
            {"timestampUnixMs", rec.timestamp_unix_ms},
            {"model", model_name},
            {"resolvedModel", resolved_model_name.empty() ? model_name : resolved_model_name},
            {"promptTokens", result.prompt_tokens},
            {"completionTokens", result.completion_tokens},
            {"durationMs", result.duration_ms},
            {"generationDurationMs", result.generation_duration_ms},
            {"tokensPerSecond", result.tokens_per_second},
            {"promptTokensPerSecond", prompt_duration_ms > 0.0
                ? static_cast<double>(evaluated_prompt_tokens) /
                    (prompt_duration_ms / 1000.0)
                : 0.0},
            {"status", status_code},
            {"inputAudioSeconds", input_audio_seconds},
            {"inputCharacters", input_characters},
        }.dump());
    }
}

void record_request(const GatewayDeps& deps,
                    const std::string& model_name,
                    const model::InferenceResult& result,
                    int status_code,
                    int slot_id,
                    double input_audio_seconds,
                    std::int64_t input_characters,
                    const std::string& resolved_model_name) {
    record_request(deps.metrics, deps.stats_db, deps.events,
                   model_name, result, status_code, slot_id,
                   input_audio_seconds, input_characters, resolved_model_name);
}

foundation::Result<ResolvedModelName> resolve_model_name(
    const GatewayDeps& deps, const std::string& requested) {
    auto resolved = deps.coordinator.registry().resolve(requested);
    if (!resolved) {
        return foundation::Err<ResolvedModelName>(resolved.error().code,
                                                  resolved.error().message);
    }
    return foundation::Ok(ResolvedModelName{
        requested, *resolved, *resolved != requested});
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
                                      const std::string& target,
                                      bool defer_resource_busy) {
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
    const bool deferred = !result && defer_resource_busy &&
        result.error().code == foundation::ErrorCode::ResourceBusy;
    LOG_INFO("swap_complete", "to={} success={} duration_ms={} error={}",
             target, result.has_value(), elapsed, error);
    if (!deferred) {
        record_swap(deps, from, target, elapsed, result.has_value(), error);
    }
    publish_model_event(deps, result ? "ready" :
        (cancelled ? "cancelled" : deferred ? "waiting" : "failed"),
                        from, target, elapsed, error);
    if (deps.swap_tracker) {
        deps.swap_tracker->end(
            result.has_value(), error, cancelled,
            result ? foundation::ErrorCode::Ok : result.error().code,
            deferred);
    }
    return result;
}

EnsureLoadedResult swap_start_error(const SwapStartResult& started) {
    const auto error = started.body.value("error", nlohmann::json::object());
    const auto code = error.value("code", "swap_start_failed");
    const auto error_code = started.status == 404
        ? foundation::ErrorCode::NotFound
        : started.status >= 500 && code == "swap_start_failed"
            ? foundation::ErrorCode::Internal
            : foundation::ErrorCode::Unavailable;
    return {
        false,
        started.status,
        code,
        error.value("message", "model swap could not be started"),
        error_code,
    };
}

std::string dump_json(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string sse_chunk_json(const std::string& id, const std::string& model,
                           std::int64_t created, const nlohmann::json& delta,
                           bool include_usage) {
    nlohmann::json chunk = {
        {"id", id},
        {"object", "chat.completion.chunk"},
        {"created", created},
        {"model", model},
        {"choices", nlohmann::json::array({
            {
                {"index", 0},
                {"delta", delta},
                {"finish_reason", nullptr},
            }
        })},
    };
    if (include_usage) chunk["usage"] = nullptr;
    return "data: " + dump_json(chunk) + "\n\n";
}

std::string sse_terminal(const std::string& id, const std::string& model,
                         std::int64_t created,
                         const std::string& finish_reason,
                         const model::InferenceResult* result,
                         bool include_usage) {
    nlohmann::json finish = {
        {"id", id},
        {"object", "chat.completion.chunk"},
        {"created", created},
        {"model", model},
        {"choices", nlohmann::json::array({
            {
                {"index", 0},
                {"delta", nlohmann::json::object()},
                {"finish_reason", finish_reason},
            }
        })},
    };
    if (include_usage) finish["usage"] = nullptr;
    std::string output = "data: " + dump_json(finish) + "\n\n";
    if (include_usage && result) {
        nlohmann::json usage = {
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"created", created},
            {"model", model},
            {"choices", nlohmann::json::array()},
            {"usage", {
            {"prompt_tokens", result->prompt_tokens},
            {"prompt_tokens_details", {{"cached_tokens", result->cached_prompt_tokens}}},
            {"completion_tokens", result->completion_tokens},
            {"total_tokens", result->prompt_tokens + result->completion_tokens},
            }},
        };
        output += "data: " + dump_json(usage) + "\n\n";
    }
    output += "data: [DONE]\n\n";
    return output;
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

foundation::Result<std::optional<std::string>> normalize_reasoning_request(
    nlohmann::json& body, const model::ModelInfo& info) {
    std::optional<std::string> requested;
    bool supplied = false;
    bool explicit_kwarg = false;
    if (body.contains("reasoning_effort")) {
        supplied = true;
        if (!body["reasoning_effort"].is_string()) {
            return foundation::Err<std::optional<std::string>>(
                foundation::ErrorCode::InvalidArgument,
                "reasoning_effort must be a string");
        }
        requested = body["reasoning_effort"].get<std::string>();
    }
    if (body.contains("chat_template_kwargs")) {
        if (!body["chat_template_kwargs"].is_object()) {
            return foundation::Err<std::optional<std::string>>(
                foundation::ErrorCode::InvalidArgument,
                "chat_template_kwargs must be an object");
        }
        auto& kwargs = body["chat_template_kwargs"];
        if (kwargs.contains("reasoning_effort")) {
            supplied = true;
            explicit_kwarg = true;
            if (!kwargs["reasoning_effort"].is_string()) {
                return foundation::Err<std::optional<std::string>>(
                    foundation::ErrorCode::InvalidArgument,
                    "chat_template_kwargs.reasoning_effort must be a string");
            }
            requested = kwargs["reasoning_effort"].get<std::string>();
        }
    }
    if (!requested && info.reasoning.supported &&
        !info.reasoning.default_effort.empty()) {
        requested = info.reasoning.default_effort;
    }
    if (!requested) return foundation::Ok(std::optional<std::string>{});
    if (!info.reasoning.supported) {
        return foundation::Err<std::optional<std::string>>(
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
            return foundation::Err<std::optional<std::string>>(
                foundation::ErrorCode::InvalidArgument,
                "reasoning effort 'none' is not supported by model: " + info.name);
        }
    } else if (std::find(info.reasoning.efforts.begin(),
                         info.reasoning.efforts.end(), resolved) ==
               info.reasoning.efforts.end()) {
        return foundation::Err<std::optional<std::string>>(
            foundation::ErrorCode::InvalidArgument,
            "unsupported reasoning effort '" + *requested + "' for model: " +
                info.name);
    }

    body["reasoning_effort"] = resolved;
    if (explicit_kwarg) {
        body["chat_template_kwargs"]["reasoning_effort"] = resolved;
    }
    LOG_INFO("reasoning_effort_resolved", "model={} effort={} source={}",
             info.name, resolved,
             explicit_kwarg ? "chat_template_kwargs"
                            : (supplied ? "protocol" : "model_default"));
    return foundation::Ok(std::optional<std::string>{std::move(resolved)});
}

nlohmann::json delta_json(const model::InferenceDelta& delta,
                          bool include_reasoning_content) {
    nlohmann::json out = nlohmann::json::object();
    if (include_reasoning_content && !delta.reasoning_text.empty()) {
        out["reasoning_content"] = delta.reasoning_text;
    }
    if (!delta.content.empty()) out["content"] = delta.content;
    if (!delta.tool_calls.empty()) {
        out["tool_calls"] = nlohmann::json::array();
        for (const auto& tc : delta.tool_calls) {
            out["tool_calls"].push_back(tool_call_delta_json(tc));
        }
    }
    return out;
}

std::size_t delta_size(const model::InferenceDelta& delta) {
    std::size_t size = delta.content.size() + delta.reasoning_text.size();
    for (const auto& call : delta.tool_calls) {
        size += call.id.size() + call.type.size() + call.function_name.size() +
                call.function_arguments.size();
    }
    return size;
}

bool content_uses_vision(const nlohmann::json& content) {
    if (!content.is_array()) return false;
    for (const auto& part : content) {
        if (!part.is_object()) continue;
        const auto type = part.value("type", "");
        if (type == "image" || type == "image_url" || type == "input_image") return true;
    }
    return false;
}

bool chat_uses_vision(const nlohmann::json& body) {
    if (!body.contains("messages") || !body["messages"].is_array()) return false;
    for (const auto& message : body["messages"]) {
        if (message.is_object() && message.contains("content") &&
            content_uses_vision(message["content"])) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string serialize_chat_stream_delta(const std::string& id,
                                        const std::string& model,
                                        std::int64_t created,
                                        const nlohmann::json& delta,
                                        bool include_usage,
                                        bool include_reasoning_content) {
    auto filtered = delta;
    if (!include_reasoning_content) filtered.erase("reasoning_content");
    return sse_chunk_json(id, model, created, filtered, include_usage);
}

std::string serialize_chat_stream_terminal(const std::string& id,
                                           const std::string& model,
                                           std::int64_t created,
                                           const std::string& finish_reason,
                                           const model::InferenceResult* result,
                                           bool include_usage) {
    return sse_terminal(id, model, created, finish_reason, result, include_usage);
}

void write_json(httplib::Response& resp, int status,
                const nlohmann::json& body) {
    resp.status = status;
    resp.set_content(dump_json(body), "application/json");
}

nlohmann::json make_error_json(int status, const std::string& code,
                               const std::string& message,
                               nlohmann::json param) {
    std::string type = "invalid_request_error";
    if (status == 401) type = "authentication_error";
    else if (status == 403) type = "permission_error";
    else if (status == 429) type = "rate_limit_error";
    else if (status >= 500) type = "server_error";
    return {
        {"error", {
            {"message", message},
            {"type", type},
            {"param", std::move(param)},
            {"code", code},
        }},
    };
}

void write_error(httplib::Response& resp, int status, const std::string& code,
                 const std::string& message, nlohmann::json param) {
    write_json(resp, status,
               make_error_json(status, code, message, std::move(param)));
}

std::string header_value(const httplib::Request& req, const std::string& name) {
    auto it = req.headers.find(name);
    if (it == req.headers.end()) return {};
    return it->second;
}

std::string request_client_key(const httplib::Request& req) {
    const auto explicit_key = header_value(req, "X-InferDeck-Voice-Session");
    if (!explicit_key.empty()) return explicit_key;
    return req.remote_addr.empty() ? "local" : req.remote_addr;
}

bool require_json_media_type(const httplib::Request& req,
                             httplib::Response& resp) {
    const std::string value = req.get_header_value("Content-Type");
    if (value.empty() && req.version.empty()) return true;
    const auto separator = value.find(';');
    const std::string media = value.substr(0, separator);
    if (media == "application/json") return true;
    write_error(resp, 415, "unsupported_media_type",
                "Content-Type must be application/json");
    return false;
}

void handle_models(const httplib::Request& req, httplib::Response& resp,
                   const GatewayDeps& deps) {
    (void)req;
    nlohmann::json data = nlohmann::json::array();
    for (const auto& name : deps.coordinator.registry().list()) {
        data.push_back({
            {"id", name},
            {"object", "model"},
            {"created", std::time(nullptr)},
            {"owned_by", "inferdeck"},
        });
    }
    for (const auto& alias : deps.coordinator.registry().aliases()) {
        data.push_back({
            {"id", alias.name},
            {"object", "model"},
            {"created", std::time(nullptr)},
            {"owned_by", "inferdeck"},
        });
    }
    nlohmann::json body = {
        {"object", "list"},
        {"data", data},
    };
    write_json(resp, 200, body);
}

bool maintenance_mode_active(const GatewayDeps& deps) noexcept {
    return deps.maintenance_resource &&
        deps.maintenance_resource->load() != ComputeResource::None;
}

ComputeResource model_compute_resource(const model::ModelInfo& info) noexcept {
    return info.compute == model::ModelCompute::Cpu
        ? ComputeResource::Cpu : ComputeResource::Gpu;
}

bool maintenance_blocks_model(const GatewayDeps& deps,
                              const std::string& model_name) noexcept {
    if (!deps.maintenance_resource) return false;
    const auto active = deps.maintenance_resource->load();
    if (active == ComputeResource::None) return false;
    const auto info = deps.coordinator.registry().get_info_result(model_name);
    return info && model_compute_resource(*info) == active;
}

SwapStartResult start_swap_async(const GatewayDeps& deps, const std::string& model_name,
                                 bool defer_resource_busy) {
    const auto resolved = resolve_model_name(deps, model_name);
    if (!resolved) {
        return {404, make_error_json(404, "model_not_found",
                                     "model not registered: " + model_name)};
    }
    const std::string target_name = resolved->resolved;
    if (maintenance_blocks_model(deps, target_name)) {
        return {503, make_error_json(
            503, "maintenance_mode",
            "measured model optimization is using the same compute resource; retry after it restores the active profile")};
    }
    const auto info = deps.coordinator.registry().get_info_result(target_name);
    if (!info || !deps.coordinator.registry().has_factory(info->runtime)) {
        return {503, make_error_json(
            503, "runtime_unavailable",
            "runtime is not linked: " +
                (info ? info->runtime : std::string("unknown")))};
    }
    auto current = deps.coordinator.get_loaded_model();
    if (deps.coordinator.is_loaded(target_name)) {
        return {200, {{"status", "ready"},
                      {"model", model_name},
                      {"resolved_model", target_name},
                      {"message", "model already loaded"}}};
    }
    if (!deps.swap_tracker) {
        return {503, make_error_json(
            503, "swap_executor_unavailable",
            "model swap executor is unavailable")};
    }

    GatewayDeps deps_copy = deps;
    const std::string from = current.value_or("");
    std::string launch_error;
    const auto start_result = deps.swap_tracker->start(
        from, target_name, now_ms(),
        [deps_copy, from, target_name, defer_resource_busy]() {
            publish_model_event(deps_copy, "swapping", from, target_name, 0.0, "");
            (void)perform_swap(deps_copy, from, target_name, defer_resource_busy);
        },
        launch_error);
    if (start_result == SwapTracker::StartResult::Busy) {
        const auto snap = deps.swap_tracker ? deps.swap_tracker->snapshot() : SwapSnapshot{};
        return {409, make_error_json(
            409, "swap_in_progress",
            "a swap to " + snap.target + " is already in progress")};
    }
    if (start_result == SwapTracker::StartResult::Failed) {
        return {500, make_error_json(
            500, "swap_start_failed",
            "model swap worker could not be started: " + launch_error)};
    }
    return {202, {{"status", "swapping"},
                  {"model", model_name},
                  {"resolved_model", target_name},
                  {"from", from}}};
}

EnsureLoadedResult ensure_model_loaded(const GatewayDeps& deps,
                                       const std::string& model_name) {
    return ensure_model_loaded(
        deps, model_name,
        std::chrono::steady_clock::now() + std::chrono::minutes{5}, {});
}

EnsureLoadedResult ensure_model_loaded(
    const GatewayDeps& deps, const std::string& model_name,
    std::chrono::steady_clock::time_point deadline,
    const std::function<bool()>& cancelled) {
    if (maintenance_blocks_model(deps, model_name)) {
        return {false, 503, "maintenance_mode",
                "measured model optimization is using the same compute resource; retry after it restores the active profile",
                foundation::ErrorCode::Unavailable};
    }
    if (deps.coordinator.is_loaded(model_name)) {
        return {true, 200, "", "", foundation::ErrorCode::Ok};
    }
    if (!deps.auto_swap) {
        return {false, 503, "model_not_loaded",
                "model not loaded; POST /api/inferdeck/v1/swap/to/" +
                    model_name + " then retry",
                foundation::ErrorCode::NotLoaded};
    }

    const auto info = deps.coordinator.registry().get_info_result(model_name);
    if (info && info->role != model::ModelRole::Conversation &&
        info->compute == model::ModelCompute::Cpu) {
        if (!deps.coordinator.registry().has_factory(info->runtime)) {
            return {false, 503, "runtime_unavailable",
                    "runtime is not linked: " + info->runtime,
                    foundation::ErrorCode::Unavailable};
        }
        auto loaded = deps.coordinator.load_with_lock_deadline(
            model_name, deadline, cancelled);
        if (loaded) return {true, 200, "", "", foundation::ErrorCode::Ok};
        return {false,
                loaded.error().code == foundation::ErrorCode::NotFound ? 404 : 503,
                "sidecar_load_failed", loaded.error().message,
                loaded.error().code};
    }

    if (!deps.swap_tracker) {
        return {false, 503, "swap_executor_unavailable",
                "model swap executor is unavailable",
                foundation::ErrorCode::Unavailable};
    }

    LOG_INFO("auto_swap_begin", "requested={}", model_name);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancelled && cancelled()) {
            return {false, 499, "request_cancelled",
                    "request cancelled while loading model: " + model_name,
                    foundation::ErrorCode::Cancelled};
        }
        if (deps.coordinator.is_loaded(model_name)) {
            return {true, 200, "", "", foundation::ErrorCode::Ok};
        }

        auto started = start_swap_async(deps, model_name, true);
        if (started.status != 200 && started.status != 202 &&
            started.status != 409) {
            return swap_start_error(started);
        }

        if (deps.coordinator.is_loaded(model_name)) {
            return {true, 200, "", "", foundation::ErrorCode::Ok};
        }
        const auto wait_deadline = std::min(
            deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds{100});
        if (!deps.swap_tracker->wait_until_idle(wait_deadline)) {
            continue;
        }

        if (deps.coordinator.is_loaded(model_name)) {
            return {true, 200, "", "", foundation::ErrorCode::Ok};
        }
        const auto snap = deps.swap_tracker->snapshot();
        if (snap.target == model_name) {
            if (snap.last_cancelled) {
                return {false, 503, "swap_cancelled",
                        "model load was cancelled: " + model_name,
                        foundation::ErrorCode::Cancelled};
            }
            if (!snap.last_error.empty()) {
                const bool capacity_busy =
                    snap.last_error_code == foundation::ErrorCode::ResourceBusy;
                return {false, 503,
                        capacity_busy ? "capacity_busy" : "swap_failed",
                        (capacity_busy ? "model load deferred: " :
                                         "model load failed: ") + snap.last_error,
                        snap.last_error_code};
            }
        }
    }
    return {false, 503, "swap_timeout", "model load timed out: " + model_name,
            foundation::ErrorCode::Timeout};
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
    const auto identities = deps.coordinator.identity_snapshot();
    const auto snap = deps.swap_tracker ? deps.swap_tracker->snapshot() : SwapSnapshot{};
    nlohmann::json body = {
        {"loaded_model", current ? *current : ""},
        {"loaded_models", deps.coordinator.get_loaded_models()},
        {"identities", {
            {"selected", identities.selected ? *identities.selected : ""},
            {"resident", identities.resident},
            {"executing", identities.executing},
        }},
        {"residency", nlohmann::json::array()},
        {"vram_usage_mb", deps.coordinator.get_vram_usage()},
        {"vram_budget_mb", deps.coordinator.vram_budget_mb()},
        {"vram_available_mb", deps.coordinator.vram_available_mb()},
        {"resource_decision", deps.coordinator.last_resource_decision()},
        {"active_requests", deps.coordinator.active_request_count()},
        {"swapping", snap.swapping},
        {"target", snap.target},
        {"from", snap.from},
        {"started_unix_ms", snap.started_unix_ms},
        {"last_error", snap.last_error},
        {"last_deferred", snap.last_deferred},
        {"last_cancelled", snap.last_cancelled},
    };
    for (const auto& resident : deps.coordinator.residency()) {
        body["residency"].push_back({
            {"name", resident.name},
            {"runtime", resident.runtime},
            {"modality", resident.modality},
            {"role", resident.role},
            {"compute", resident.compute},
            {"residency_policy", resident.residency},
            {"admission_pool", resident.admission_pool},
            {"concurrency_limit", resident.concurrency_limit},
            {"memory_required_mb", resident.memory_required_mb},
            {"eviction_eligible", resident.eviction_eligible},
            {"slots", resident.slots},
            {"free_slots", resident.free_slots},
            {"active_requests", resident.active_requests},
            {"estimated_vram_mb", resident.estimated_vram_mb},
            {"primary", resident.primary},
            {"resizing", resident.resizing},
        });
    }
    write_json(resp, 200, body);
}

namespace {

struct AcquiredChatSlot {
    int slot_id{-1};
    std::string reservation_key;
    std::optional<std::uint64_t> voice_session_token;
};

std::optional<AcquiredChatSlot> acquire_chat_slot(
    const httplib::Request& req, httplib::Response& resp,
    const GatewayDeps& deps, int priority,
    const std::string& requested_model, const std::string& model_name) {
    AcquiredChatSlot acquired;
    acquired.reservation_key = request_client_key(req);
    acquired.voice_session_token = deps.coordinator.hold_priority_session(
        acquired.reservation_key, model_name);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::minutes{5};
    const std::function<bool()> cancelled = [&req] {
        return req.is_connection_closed();
    };
    model::AcquireSlotOptions opts;
    opts.timeout = std::chrono::minutes{5};
    opts.block = true;
    opts.priority = std::clamp(priority, -100, 100);
    opts.reservation_key = acquired.reservation_key;
    if (acquired.voice_session_token) opts.priority = 100;
    opts.cancelled = cancelled;
    opts.prepare = [&deps, &model_name, deadline, cancelled] {
        auto loaded = ensure_model_loaded(deps, model_name, deadline, cancelled);
        if (loaded.ok) return foundation::Ok();
        return foundation::Err<void>(loaded.error_code, loaded.message);
    };
    auto slot = deps.coordinator.acquire_slot(model_name, opts);
    if (slot) {
        acquired.slot_id = *slot;
        return acquired;
    }
    if (acquired.voice_session_token) {
        deps.coordinator.complete_priority_session_hold(
            acquired.reservation_key, *acquired.voice_session_token,
            std::chrono::milliseconds{deps.voice_session_grace_ms});
    }
    int status = 503;
    std::string code = "no_slots";
    if (slot.error().code == foundation::ErrorCode::Timeout) {
        code = "slot_timeout";
    } else if (slot.error().code == foundation::ErrorCode::Cancelled) {
        code = "cancelled";
    } else if (slot.error().code == foundation::ErrorCode::NotFound) {
        status = 404;
        code = "model_not_loaded";
    } else if (slot.error().code == foundation::ErrorCode::NotLoaded) {
        code = "model_not_loaded";
    }
    resp.set_header("Retry-After",
        slot.error().code == foundation::ErrorCode::NotLoaded
            ? deps.default_swap_timeout_s : "1");
    model::InferenceResult failed;
    record_request(deps, requested_model, failed, status, -1, 0.0, 0, model_name);
    write_error(resp, status, code, slot.error().message);
    return std::nullopt;
}

void handle_non_stream_chat(
    httplib::Response& resp, const GatewayDeps& deps,
    const std::string& requested_model, const std::string& model_name,
    const std::string& id, GenerationSession& session,
    const model::InferenceRequest& inference_request) {
    auto predicted = session.run(inference_request);
    if (!predicted) {
        const auto error = map_openai_error(predicted.error().code);
        LOG_ERROR("inference_failed",
                  "model={} slot_id={} status={} code={} error={}",
                  model_name, session.slot_id, error.status, error.code,
                  predicted.error().message);
        write_error(resp, error.status, error.code, predicted.error().message);
        session.finish_once(false, error.status, "inference_error");
        return;
    }
    const auto& result = *predicted;
    nlohmann::json message = {
        {"role", "assistant"},
        {"content", result.text},
    };
    if (deps.compatibility_profile == CompatibilityProfile::OpenAIDerivative &&
        !result.reasoning_text.empty()) {
        message["reasoning_content"] = result.reasoning_text;
    }
    if (!result.tool_calls.empty()) {
        message["tool_calls"] = nlohmann::json::array();
        for (const auto& call : result.tool_calls) {
            message["tool_calls"].push_back(tool_call_json(call));
        }
    }
    const std::string finish_reason = !result.tool_calls.empty()
        ? "tool_calls" : result.finish_reason;
    write_json(resp, 200, {
        {"id", id},
        {"object", "chat.completion"},
        {"created", std::time(nullptr)},
        {"model", requested_model},
        {"choices", nlohmann::json::array({{
            {"index", 0},
            {"message", message},
            {"finish_reason", finish_reason},
        }})},
        {"usage", {
            {"prompt_tokens", result.prompt_tokens},
            {"prompt_tokens_details", {
                {"cached_tokens", result.cached_prompt_tokens},
            }},
            {"completion_tokens", result.completion_tokens},
            {"total_tokens", result.prompt_tokens + result.completion_tokens},
        }},
    });
    session.finish_once(false, 200, "completed");
}

} // namespace

std::optional<AcquiredGenerationSlot> acquire_generation_slot(
    const httplib::Request& req, httplib::Response& resp,
    const GatewayDeps& deps, int priority,
    const std::string& requested_model, const std::string& resolved_model) {
    auto acquired = acquire_chat_slot(req, resp, deps, priority,
                                      requested_model, resolved_model);
    if (!acquired) return std::nullopt;
    return AcquiredGenerationSlot{
        acquired->slot_id,
        std::move(acquired->reservation_key),
        std::move(acquired->voice_session_token),
    };
}

void handle_chat_completions(const httplib::Request& req, httplib::Response& resp,
                             const GatewayDeps& deps) {
    if (!require_json_media_type(req, resp)) return;
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        write_error(resp, 400, "invalid_json", e.what());
        return;
    }
    if (!body.contains("model") || !body["model"].is_string()) {
        write_error(resp, 400, "missing_model",
                    "request body must include 'model'", "model");
        return;
    }
    const bool derivative =
        deps.compatibility_profile == CompatibilityProfile::OpenAIDerivative;
    if (!derivative) {
        static constexpr std::array<std::string_view, 15> supported_fields{
            "model", "messages", "max_tokens", "max_completion_tokens",
            "temperature", "top_p", "seed", "stream", "stream_options",
            "tools", "tool_choice", "parallel_tool_calls", "reasoning_effort",
            "response_format", "stop",
        };
        for (const auto& field : body.items()) {
            if (std::find(supported_fields.begin(), supported_fields.end(),
                          field.key()) == supported_fields.end()) {
                write_error(resp, 400, "unsupported_parameter",
                            "unsupported Chat Completions parameter: " +
                                field.key(),
                            field.key());
                return;
            }
        }
        if (body.contains("messages") && body["messages"].is_array()) {
            for (const auto& message : body["messages"]) {
                if (message.is_object() && message.contains("reasoning_content")) {
                    write_error(resp, 400, "unsupported_parameter",
                                "unsupported Chat Completions message parameter: reasoning_content",
                                "messages");
                    return;
                }
            }
        }
    }
    if (body.contains("stream") && !body["stream"].is_boolean()) {
        write_error(resp, 400, "invalid_request_error",
                    "stream must be a boolean", "stream");
        return;
    }
    const bool stream = body.value("stream", false);
    if (body.contains("stream_options") &&
        !body["stream_options"].is_object()) {
        write_error(resp, 400, "invalid_request_error",
                    "stream_options must be an object", "stream_options");
        return;
    }
    if (!stream && body.contains("stream_options")) {
        write_error(resp, 400, "invalid_request_error",
                    "stream_options requires stream to be true",
                    "stream_options");
        return;
    }
    if (body.contains("stream_options")) {
        static constexpr std::array<std::string_view, 1>
            stream_option_fields{"include_usage"};
        for (const auto& field : body["stream_options"].items()) {
            if (std::find(stream_option_fields.begin(),
                          stream_option_fields.end(), field.key()) ==
                stream_option_fields.end()) {
                write_error(
                    resp, 400, "unsupported_parameter",
                    "unsupported stream_options parameter: " + field.key(),
                    "stream_options");
                return;
            }
        }
        if (body["stream_options"].contains("include_usage") &&
            !body["stream_options"]["include_usage"].is_boolean()) {
            write_error(resp, 400, "invalid_request_error",
                        "stream_options.include_usage must be a boolean",
                        "stream_options");
            return;
        }
    }
    const bool include_stream_usage =
        body.contains("stream_options") &&
        body["stream_options"].value("include_usage", false);
    auto inference_request =
        parse_openai_chat_request(body, derivative);
    if (!inference_request) {
        write_error(resp, 400, "invalid_request_error",
                    inference_request.error().message,
                    inference_request.error().field.empty()
                        ? nlohmann::json(nullptr)
                        : nlohmann::json(inference_request.error().field));
        return;
    }
    std::string requested_model = body["model"].get<std::string>();
    const auto resolved_model = resolve_model_name(deps, requested_model);
    if (!resolved_model) {
        write_error(resp, 404, "model_not_found", resolved_model.error().message);
        return;
    }
    const std::string& model_name = resolved_model->resolved;
    if (maintenance_blocks_model(deps, model_name)) {
        write_error(resp, 503, "maintenance_mode",
                    "measured model optimization is using the same compute resource");
        return;
    }
    const auto model_info = deps.coordinator.registry().get_info_result(model_name);
    if (!model_info) {
        write_error(resp, 404, "model_not_found", model_info.error().message);
        return;
    }
    if (!model_info->has_vision && chat_uses_vision(body)) {
        write_error(resp, 400, "unsupported_capability",
                    "model does not support image input: " + model_name);
        return;
    }
    auto reasoning_effort = normalize_reasoning_request(body, *model_info);
    if (!reasoning_effort) {
        write_error(resp, 400, "invalid_request_error",
                    reasoning_effort.error().message);
        return;
    }
    inference_request->reasoning_effort = *reasoning_effort;

    const int priority = derivative && body.contains("priority") &&
            body["priority"].is_number_integer()
        ? body["priority"].get<int>() : 0;
    auto acquired = acquire_generation_slot(
        req, resp, deps, priority, requested_model, model_name);
    if (!acquired) return;
    const int slot_id = acquired->slot_id;
    const auto& reservation_key = acquired->reservation_key;
    const auto& voice_session_token = acquired->voice_session_token;

    const std::string id = make_id();
    const std::string stream_model = requested_model;
    const auto stream_created = static_cast<std::int64_t>(std::time(nullptr));

    auto state = std::make_shared<GenerationSession>(
        deps.coordinator, deps.metrics, deps.stats_db, deps.events,
        slot_id, requested_model, model_name, reservation_key,
        voice_session_token.value_or(0), deps.voice_session_grace_ms);

    if (!stream) {
        handle_non_stream_chat(resp, deps, requested_model, model_name,
                               id, *state, *inference_request);
        return;
    }

    resp.set_header("Content-Type", "text/event-stream");
    resp.set_header("Cache-Control", "no-cache");
    resp.set_header("Connection", "keep-alive");
    state->start(std::move(*inference_request));

    resp.set_chunked_content_provider(
        "text/event-stream",
        [id, stream_model, stream_created, state, include_stream_usage,
         derivative](
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
                    state->pending_bytes -= delta_size(state->delta_queue.front());
                    deltas.push_back(std::move(state->delta_queue.front()));
                    state->delta_queue.pop_front();
                }
                lk.unlock();
                state->cv.notify_all();

                if (!sink.is_writable()) {
                    LOG_WARN("stream_abort", "model={} slot_id={} reason=sink_not_writable",
                             state->model_name, state->slot_id);
                    state->finish_once(true, 499, "sink_not_writable");
                    return false;
                }

                for (const auto& delta : deltas) {
                    auto json_delta = delta_json(state->utf8.on_delta(delta), derivative);
                    if (json_delta.empty()) continue;
                    std::string out = serialize_chat_stream_delta(
                        id, stream_model, stream_created, json_delta,
                        include_stream_usage, derivative);
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

            auto trailing_delta = delta_json(state->utf8.finish(), derivative);
            if (!trailing_delta.empty()) {
                std::string out = serialize_chat_stream_delta(
                    id, stream_model, stream_created, trailing_delta,
                    include_stream_usage, derivative);
                if (!sink.write(out.data(), out.size())) {
                    LOG_WARN("stream_abort", "model={} slot_id={} reason=trailing_chunk_write_failed",
                             state->model_name, state->slot_id);
                    state->finish_once(true, 499, "trailing_chunk_write_failed");
                    return false;
                }
            }

            if (inference_error) {
                const auto ec = map_openai_error(error_code);
                LOG_ERROR("inference_failed",
                          "model={} slot_id={} status={} code={} error={}",
                          state->model_name, state->slot_id, ec.status, ec.code, error_msg);
                auto error = make_error_json(ec.status, ec.code, error_msg);
                error["error"]["type"] = ec.type;
                std::string err = "data: " + dump_json(error) +
                    "\n\ndata: [DONE]\n\n";
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
                std::string done = serialize_chat_stream_terminal(
                    id, stream_model, stream_created, finish_reason,
                    final_result ? final_result.get() : nullptr,
                    include_stream_usage);
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
