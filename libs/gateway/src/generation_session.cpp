#include "gateway/generation_session.hpp"

#include "foundation/logging.hpp"
#include "gateway/routes.hpp"

#include <chrono>
#include <exception>

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

}

GenerationSession::GenerationSession(
    model::BackendCoordinator& coordinator_value,
    observability::Metrics* metrics_value,
    observability::StatsDb* stats_db_value,
    foundation::EventBus* events_value,
    int slot_id_value,
    std::string requested_model_value,
    std::string resolved_model_value,
    std::string reservation_key_value,
    std::uint64_t voice_session_token_value,
    int voice_session_grace_ms_value,
    RequestObservation observation_value)
    : slot_id(slot_id_value),
      model_name(std::move(resolved_model_value)),
      requested_model(std::move(requested_model_value)),
      coordinator(&coordinator_value),
      metrics(metrics_value),
      stats_db(stats_db_value),
      events(events_value),
      reservation_key(std::move(reservation_key_value)),
      voice_session_token(voice_session_token_value),
      voice_session_grace_ms(voice_session_grace_ms_value),
      observation(std::move(observation_value)) {
    if (observation.live) {
        observation.request_id = observation.live->request_id;
        observation.api_key_id = observation.live->api_key_id;
        observation.api_key_name = observation.live->api_key_name;
    }
}

GenerationSession::~GenerationSession() {
    finish_once(true, 499, "session_destroyed");
}

void GenerationSession::start(model::InferenceRequest request, bool stream) {
    if (observation.live) request.progress = observation.live->progress;
    inference_thread = std::thread([this, request = std::move(request), stream]() {
        try {
            auto result = stream ? coordinator->predict_stream(
                model_name, slot_id, request,
                [this](const model::InferenceDelta& delta) {
                    if (aborted.load()) return false;
                    const auto bytes = delta_size(delta);
                    std::unique_lock lock(mtx);
                    cv.wait(lock, [this, bytes] {
                        const bool byte_capacity = delta_queue.empty() ||
                            (bytes <= max_pending_stream_bytes &&
                             pending_bytes <= max_pending_stream_bytes - bytes);
                        return aborted.load() ||
                            (delta_queue.size() < max_pending_stream_deltas &&
                             byte_capacity);
                    });
                    if (aborted.load()) return false;
                    delta_queue.push_back(delta);
                    pending_bytes += bytes;
                    lock.unlock();
                    cv.notify_one();
                    return !aborted.load();
                },
                &aborted)
                : coordinator->predict(model_name, slot_id, request, &aborted);
            {
                std::lock_guard lock(mtx);
                if (result) {
                    final_result = std::make_shared<model::InferenceResult>(
                        std::move(*result));
                } else {
                    inference_error = true;
                    error_code = result.error().code;
                    error_msg = result.error().message;
                }
                inference_done = true;
            }
            cv.notify_all();
        } catch (const std::exception& error) {
            LOG_ERROR("inference_thread_exception", "model={} slot_id={} what={}",
                      model_name, slot_id, error.what());
            std::lock_guard lock(mtx);
            inference_error = true;
            error_msg = error.what();
            inference_done = true;
            cv.notify_all();
        } catch (...) {
            LOG_ERROR("inference_thread_exception", "model={} slot_id={} what=unknown",
                      model_name, slot_id);
            std::lock_guard lock(mtx);
            inference_error = true;
            error_msg = "unknown exception";
            inference_done = true;
            cv.notify_all();
        }
    });
}

foundation::Result<model::InferenceResult> GenerationSession::run(
    const model::InferenceRequest& request, const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) aborted.store(true);
    start(request, false);
    std::unique_lock lock(mtx);
    while (!inference_done) {
        lock.unlock();
        if (cancelled && cancelled()) aborted.store(true);
        lock.lock();
        cv.wait_for(lock, std::chrono::milliseconds{50}, [this] {
            return inference_done;
        });
    }
    lock.unlock();
    if (inference_thread.joinable()) inference_thread.join();
    if (cancelled && cancelled()) aborted.store(true);
    if (aborted.load()) {
        observation.error_code = "cancelled";
        return foundation::Err<model::InferenceResult>(
            foundation::ErrorCode::Cancelled, "request cancelled");
    }
    if (inference_error) {
        return foundation::Err<model::InferenceResult>(error_code, error_msg);
    }
    return *final_result;
}

void GenerationSession::finish_once(bool aborted_stream, int fallback_status,
                                    const std::string& reason) {
    bool expected = false;
    if (!cleanup_done.compare_exchange_strong(expected, true)) return;
    aborted_stream = aborted_stream || aborted.load();
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
        std::lock_guard lock(mtx);
        result = final_result;
        error = inference_error;
    }
    int status = fallback_status;
    if (result && !aborted_stream && !error) {
        status = 200;
        record_request(metrics, stats_db, events, requested_model, *result,
                       status, slot_id, 0.0, 0, model_name, observation);
    } else {
        status = aborted_stream ? 499
            : (error && fallback_status < 400 ? 500 : fallback_status);
        record_request(metrics, stats_db, events, requested_model,
                       result ? *result : model::InferenceResult{}, status, slot_id,
                       0.0, 0, model_name, observation);
    }
    if (observation.live) observation.live->finished.store(true);
    LOG_INFO("stream_recorded", "model={} slot_id={} status={} reason={}",
             model_name, slot_id, status, reason);
    if (coordinator) {
        auto released = coordinator->release_slot(model_name, slot_id);
        if (!released) {
            LOG_WARN("stream_cleanup_release_failed", "model={} slot_id={} reason={}",
                     model_name, slot_id, released.error().message);
        }
        if (voice_session_token != 0) {
            coordinator->complete_priority_session_hold(
                reservation_key, voice_session_token,
                std::chrono::milliseconds{voice_session_grace_ms});
        }
    }
    LOG_INFO("stream_cleanup", "model={} slot_id={} aborted={} reason={}",
             model_name, slot_id, aborted_stream, reason);
}

}
