#include "gateway/routes.hpp"

#include "gateway/openai_adapter.hpp"
#include "gateway/openai_error.hpp"
#include "gateway/generation_session.hpp"
#include "gateway/request_id.hpp"
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

RequestObservation observe_request(const httplib::Request& req,
                                   const httplib::Response& resp,
                                   const GatewayDeps& deps,
                                   std::string modality,
                                   bool stream) {
    RequestObservation observation;
    observation.request_id = resp.get_header_value("X-Request-Id");
    if (observation.request_id.empty()) {
        observation.request_id = request_id(header_value(req, "X-Request-Id"));
    }
    observation.principal_class = "openai_data_plane";
    observation.endpoint = req.path;
    observation.protocol_profile =
        deps.compatibility_profile == CompatibilityProfile::OpenAIDerivative
            ? "openai_derivative" : "strict_openai";
    observation.modality = std::move(modality);
    observation.stream = stream;
    return observation;
}

void record_request(observability::Metrics* metrics,
                    observability::StatsDb* stats_db,
                    foundation::EventBus* events,
                    const std::string& model_name,
                    const model::InferenceResult& result,
                    int status_code,
                    int slot_id,
                    double input_audio_seconds,
                    std::int64_t input_characters,
                    const std::string& resolved_model_name,
                    const RequestObservation& observation) {
    observability::RequestRecord rec;
    rec.timestamp_unix_ms = now_ms();
    rec.model = model_name;
    rec.resolved_model = resolved_model_name.empty() ? rec.model : resolved_model_name;
    rec.request_id = observation.request_id;
    rec.principal_class = observation.principal_class;
    rec.endpoint = observation.endpoint;
    rec.protocol_profile = observation.protocol_profile;
    rec.modality = observation.modality;
    rec.stream = observation.stream;
    rec.finish_code = result.finish_reason;
    rec.error_code = observation.error_code;
    rec.prompt_tokens = std::max(0, result.prompt_tokens);
    rec.cached_prompt_tokens = std::clamp(result.cached_prompt_tokens, 0, rec.prompt_tokens);
    rec.cache_write_tokens = rec.prompt_tokens - rec.cached_prompt_tokens;
    rec.completion_tokens = std::max(0, result.completion_tokens);
    rec.reasoning_tokens = std::max(0, result.reasoning_tokens);
    rec.duration_ms = std::max(0.0, static_cast<double>(result.duration_ms));
    rec.generation_duration_ms = std::max(0.0, static_cast<double>(result.generation_duration_ms));
    rec.tokens_per_second = rec.modality == "text" && rec.generation_duration_ms > 0.0
        ? rec.completion_tokens * 1000.0 / rec.generation_duration_ms
        : 0.0;
    rec.status_code = status_code;
    rec.slot_id = slot_id;
    const double prompt_duration_ms = result.prompt_duration_ms > 0.0f
        ? result.prompt_duration_ms
        : std::max(0.0, static_cast<double>(
              result.duration_ms - result.generation_duration_ms));
    rec.prompt_duration_ms = prompt_duration_ms;
    rec.prompt_tokens_per_second = rec.prompt_duration_ms > 0.0
        ? rec.cache_write_tokens * 1000.0 / rec.prompt_duration_ms
        : 0.0;
    rec.queue_duration_ms = std::max(0.0, observation.queue_duration_ms);
    rec.swap_load_duration_ms = std::max(0.0, observation.swap_load_duration_ms);
    rec.first_token_duration_ms = result.first_token_duration_ms > 0.0f
        ? result.first_token_duration_ms
        : std::max(0.0, observation.first_token_duration_ms);
    rec.input_audio_seconds = std::max(0.0, input_audio_seconds);
    rec.output_audio_seconds = std::max(0.0, observation.output_audio_seconds);
    rec.input_characters = std::max<std::int64_t>(0, input_characters);
    rec.input_image_count = std::max(0, observation.input_image_count);
    rec.output_image_count = std::max(0, observation.output_image_count);
    if (metrics) metrics->record_request(rec);
    LOG_INFO("request_recorded",
             "request_id={} endpoint={} profile={} modality={} model={} resolved_model={} status={} finish_code={} error_code={} slot_id={} prompt_tokens={} cached_prompt_tokens={} completion_tokens={} duration_ms={} generation_duration_ms={} tps={}",
             rec.request_id,
             rec.endpoint,
             rec.protocol_profile,
             rec.modality,
             model_name,
             rec.resolved_model,
             status_code,
             rec.finish_code,
             rec.error_code,
             slot_id,
             result.prompt_tokens,
             result.cached_prompt_tokens,
             result.completion_tokens,
             result.duration_ms,
             result.generation_duration_ms,
             rec.tokens_per_second);
    if (stats_db) stats_db->record_request(rec);
    if (events) {
        events->publish("request", nlohmann::json{
            {"timestampUnixMs", rec.timestamp_unix_ms},
            {"requestId", rec.request_id},
            {"principalClass", rec.principal_class},
            {"endpoint", rec.endpoint},
            {"protocolProfile", rec.protocol_profile},
            {"modality", rec.modality},
            {"model", rec.model},
            {"resolvedModel", rec.resolved_model},
            {"stream", rec.stream},
            {"finishCode", rec.finish_code},
            {"errorCode", rec.error_code},
            {"promptTokens", rec.prompt_tokens},
            {"cachedPromptTokens", rec.cached_prompt_tokens},
            {"cacheWriteTokens", rec.cache_write_tokens},
            {"completionTokens", rec.completion_tokens},
            {"reasoningTokens", rec.reasoning_tokens},
            {"durationMs", rec.duration_ms},
            {"generationDurationMs", rec.generation_duration_ms},
            {"promptDurationMs", rec.prompt_duration_ms},
            {"firstTokenDurationMs", rec.first_token_duration_ms},
            {"queueDurationMs", rec.queue_duration_ms},
            {"swapLoadDurationMs", rec.swap_load_duration_ms},
            {"tokensPerSecond", rec.tokens_per_second},
            {"promptTokensPerSecond", rec.prompt_tokens_per_second},
            {"status", status_code},
            {"inputAudioSeconds", rec.input_audio_seconds},
            {"outputAudioSeconds", rec.output_audio_seconds},
            {"inputCharacters", rec.input_characters},
            {"inputImageCount", rec.input_image_count},
            {"outputImageCount", rec.output_image_count},
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
                    const std::string& resolved_model_name,
                    const RequestObservation& observation) {
    record_request(deps.metrics, deps.stats_db, deps.events,
                   model_name, result, status_code, slot_id,
                   input_audio_seconds, input_characters, resolved_model_name,
                   observation);
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
    const auto session = header_value(req, "X-InferDeck-Voice-Session");
    if (session.size() < 8 || session.size() > 128 ||
        !std::all_of(session.begin(), session.end(), [](unsigned char value) {
            return std::isalnum(value) || value == '-' || value == '_' || value == '.';
        })) {
        return {};
    }
    const auto authorization = header_value(req, "Authorization");
    if (!authorization.starts_with("Bearer ") || authorization.size() <= 7) {
        return {};
    }
    return authorization.substr(7) + '\x1f' + session;
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

#include "chat_routes.ipp"

} // namespace inferdeck::gateway
