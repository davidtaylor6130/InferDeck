#pragma once

#include "foundation/result.hpp"
#include "gateway/streaming_sanitizer.hpp"
#include "model/backend_coordinator.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace inferdeck::foundation { class EventBus; }
namespace inferdeck::observability { class Metrics; class StatsDb; }

namespace inferdeck::gateway {

class GenerationSession final {
public:
    GenerationSession(model::BackendCoordinator& coordinator,
                      observability::Metrics* metrics,
                      observability::StatsDb* stats_db,
                      foundation::EventBus* events,
                      int slot_id,
                      std::string requested_model,
                      std::string resolved_model,
                      std::string reservation_key,
                      std::uint64_t voice_session_token,
                      int voice_session_grace_ms);
    ~GenerationSession();

    GenerationSession(const GenerationSession&) = delete;
    GenerationSession& operator=(const GenerationSession&) = delete;

    void start(model::InferenceRequest request);
    foundation::Result<model::InferenceResult> run(
        const model::InferenceRequest& request);
    void finish_once(bool aborted_stream, int fallback_status,
                     const std::string& reason);

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
    std::string reservation_key;
    std::uint64_t voice_session_token{0};
    int voice_session_grace_ms{0};
    InferenceDeltaUtf8Buffer utf8;
};

}
