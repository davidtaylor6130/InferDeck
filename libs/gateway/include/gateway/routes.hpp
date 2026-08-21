#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "foundation/event_bus.hpp"
#include "gateway/swap_tracker.hpp"
#include "model/backend_coordinator.hpp"
#include "model/model_registry.hpp"
#include "observability/metrics.hpp"
#include "observability/stats_db.hpp"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace inferdeck::gateway {

enum class ComputeResource : std::uint8_t {
    None,
    Cpu,
    Gpu,
};

using CompatibilityProfile = inference::CompatibilityProfile;

struct GatewayDeps {
    model::BackendCoordinator& coordinator;
    std::string default_swap_timeout_s{"15"};
    bool auto_swap{true};
    std::string default_model{};
    int voice_session_grace_ms{15000};
    observability::Metrics* metrics{nullptr};
    observability::StatsDb* stats_db{nullptr};
    foundation::EventBus* events{nullptr};
    SwapTracker* swap_tracker{nullptr};
    std::atomic<ComputeResource>* maintenance_resource{nullptr};
    CompatibilityProfile compatibility_profile{CompatibilityProfile::StrictOpenAI};
};

struct RequestObservation {
    std::string request_id;
    std::string principal_class;
    std::string endpoint;
    std::string protocol_profile;
    std::string modality{"text"};
    bool stream{false};
    std::string error_code;
    double queue_duration_ms{};
    double swap_load_duration_ms{};
    double first_token_duration_ms{};
    double output_audio_seconds{};
    int input_image_count{};
    int output_image_count{};
};

RequestObservation observe_request(const httplib::Request& req,
                                   const httplib::Response& resp,
                                   const GatewayDeps& deps,
                                   std::string modality,
                                   bool stream);

bool maintenance_mode_active(const GatewayDeps& deps) noexcept;
ComputeResource model_compute_resource(const model::ModelInfo& info) noexcept;
bool maintenance_blocks_model(const GatewayDeps& deps,
                              const std::string& model_name) noexcept;

void record_request(observability::Metrics* metrics,
                    observability::StatsDb* stats_db,
                    foundation::EventBus* events,
                    const std::string& model_name,
                    const model::InferenceResult& result,
                    int status_code,
                    int slot_id,
                    double input_audio_seconds = 0.0,
                    std::int64_t input_characters = 0,
                    const std::string& resolved_model_name = {},
                    const RequestObservation& observation = {});
void record_request(const GatewayDeps& deps,
                    const std::string& model_name,
                    const model::InferenceResult& result,
                    int status_code,
                    int slot_id,
                    double input_audio_seconds = 0.0,
                    std::int64_t input_characters = 0,
                    const std::string& resolved_model_name = {},
                    const RequestObservation& observation = {});

struct ResolvedModelName {
    std::string requested;
    std::string resolved;
    bool alias{false};
};

foundation::Result<ResolvedModelName> resolve_model_name(
    const GatewayDeps& deps, const std::string& requested);

void write_json(httplib::Response& resp, int status, const nlohmann::json& body);
nlohmann::json make_error_json(int status, const std::string& code,
                               const std::string& message,
                               nlohmann::json param = nullptr);
void write_error(httplib::Response& resp, int status, const std::string& code,
                 const std::string& message,
                 nlohmann::json param = nullptr);
std::string serialize_chat_stream_delta(const std::string& id,
                                        const std::string& model,
                                        std::int64_t created,
                                        const nlohmann::json& delta,
                                        bool include_usage,
                                        bool include_reasoning_content = false);
std::string serialize_chat_stream_terminal(const std::string& id,
                                           const std::string& model,
                                           std::int64_t created,
                                           const std::string& finish_reason,
                                           const model::InferenceResult* result,
                                           bool include_usage);
std::string header_value(const httplib::Request& req, const std::string& name);
std::string request_client_key(const httplib::Request& req);
bool require_json_media_type(const httplib::Request& req,
                             httplib::Response& resp);

struct AcquiredGenerationSlot {
    int slot_id{-1};
    std::string reservation_key;
    std::optional<std::uint64_t> voice_session_token;
    double queue_duration_ms{};
    double swap_load_duration_ms{};
};

std::optional<AcquiredGenerationSlot> acquire_generation_slot(
    const httplib::Request& req, httplib::Response& resp,
    const GatewayDeps& deps, int priority,
    const std::string& requested_model, const std::string& resolved_model);

struct SwapStartResult {
    int status{200};
    nlohmann::json body;
};

SwapStartResult start_swap_async(const GatewayDeps& deps, const std::string& model_name,
                                 bool defer_resource_busy = false);

struct EnsureLoadedResult {
    bool ok{false};
    int status{503};
    std::string code{};
    std::string message{};
    foundation::ErrorCode error_code{foundation::ErrorCode::Unavailable};
};

// Ensure `model_name` is loaded, waiting out any in-progress swap (e.g. two
// concurrent requests racing on a cold model) rather than returning 503. On
// failure, `status`/`code`/`message` describe the error for the caller to emit.
EnsureLoadedResult ensure_model_loaded(const GatewayDeps& deps,
                                       const std::string& model_name);
EnsureLoadedResult ensure_model_loaded(
    const GatewayDeps& deps, const std::string& model_name,
    std::chrono::steady_clock::time_point deadline,
    const std::function<bool()>& cancelled);

void handle_models(const httplib::Request& req, httplib::Response& resp,
                   const GatewayDeps& deps);

void handle_swap_to(const httplib::Request& req, httplib::Response& resp,
                    const GatewayDeps& deps, const std::string& model_name);

void handle_swap_cancel(const httplib::Request& req, httplib::Response& resp,
                        const GatewayDeps& deps);

void handle_swap_status(const httplib::Request& req, httplib::Response& resp,
                        const GatewayDeps& deps);

void handle_chat_completions(const httplib::Request& req, httplib::Response& resp,
                             const GatewayDeps& deps);

} // namespace inferdeck::gateway
