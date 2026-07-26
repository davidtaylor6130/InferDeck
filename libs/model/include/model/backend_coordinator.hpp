#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "foundation/result.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"

namespace inferdeck::model {

struct AcquireSlotOptions {
    std::chrono::milliseconds timeout{30000};
    bool block{true};
    int priority{0};
    std::function<bool()> cancelled{};
    std::function<foundation::Result<void>()> prepare{};
};

struct QueueInfo {
    std::uint64_t id{0};
    std::string model;
    int priority{0};
    std::size_t position{0};
    std::int64_t queued_ms{0};
    std::int64_t remaining_ms{0};
};

struct ResidencyInfo {
    std::string name;
    std::string runtime;
    std::string modality;
    int slots{0};
    int free_slots{0};
    int active_requests{0};
    int estimated_vram_mb{0};
    bool primary{false};
    bool resizing{false};
};

class BackendCoordinator {
public:
    explicit BackendCoordinator(ModelRegistry& registry);

    foundation::Result<void> register_existing(const ModelInfo& info);
    foundation::Result<void> unregister(const std::string& name);

    foundation::Result<void> load(const std::string& name);
    foundation::Result<void> unload_current();
    foundation::Result<void> unload(const std::string& name);

    foundation::Result<void> ensure_loaded(const std::string& name);
    foundation::Result<void> swap_to(const std::string& name);

    [[nodiscard]] bool is_loaded(const std::string& name) const;
    [[nodiscard]] std::optional<std::string> get_loaded_model() const;
    [[nodiscard]] std::vector<std::string> get_loaded_models() const;
    [[nodiscard]] std::vector<ResidencyInfo> residency() const;
    [[nodiscard]] int get_vram_usage() const;
    void set_vram_budget(int total_mb, int safety_margin_mb = 1024);
    [[nodiscard]] int vram_budget_mb() const;
    [[nodiscard]] int vram_available_mb() const;
    [[nodiscard]] std::string last_resource_decision() const;
    [[nodiscard]] const IBackend* get_backend(const std::string& name) const;
    [[nodiscard]] const IModel* get_model(const std::string& name) const;

    [[nodiscard]] ModelRegistry& registry() noexcept { return registry_; }

    foundation::Result<int> acquire_slot(
        const std::string& name, const AcquireSlotOptions& opts = {});
    foundation::Result<void> release_slot(
        const std::string& name, int slot_id);
    void set_max_queue_size(std::size_t size);
    [[nodiscard]] std::size_t queued_request_count() const;
    [[nodiscard]] std::vector<QueueInfo> queue() const;

    foundation::Result<InferenceResult> predict(
        const std::string& name, int slot_id, const InferenceRequest& req);

    foundation::Result<InferenceResult> predict_stream(
        const std::string& name, int slot_id, const InferenceRequest& req,
        const IModel::TokenCallback& callback,
        const std::atomic<bool>* cancel = nullptr);

    foundation::Result<EmbeddingResult> embed(
        const std::string& name, int slot_id, const EmbeddingRequest& request,
        const std::function<bool()>& cancelled = {});
    foundation::Result<ImageGenerationResult> generate_images(
        const std::string& name, int slot_id, const ImageGenerationRequest& request,
        const std::function<bool(int)>& progress = {});
    foundation::Result<AudioResult> synthesize(
        const std::string& name, int slot_id, const SpeechRequest& request,
        const std::function<bool(const std::byte*, std::size_t)>& stream = {});
    foundation::Result<TranscriptionResult> transcribe(
        const std::string& name, int slot_id, const TranscriptionRequest& request,
        const std::function<bool(int)>& progress = {});

    void drain_active(std::chrono::milliseconds timeout = std::chrono::milliseconds{30000});
    int active_request_count() const;
    int active_request_count(const std::string& name) const;

    bool swap_in_progress() const noexcept { return swap_in_progress_.load(); }
    void request_swap_cancel() noexcept { swap_cancel_.store(true); }
    void reset_swap_cancel() noexcept { swap_cancel_.store(false); }
    bool swap_cancel_requested() const noexcept { return swap_cancel_.load(); }

    foundation::Result<void> swap_to_cancellable(const std::string& name,
                                                 std::chrono::milliseconds timeout = std::chrono::milliseconds{30000});

private:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    struct SlotWaiter {
        std::uint64_t id{0};
        std::string model;
        int priority{0};
        time_point enqueued{};
        time_point deadline{};
        std::function<bool()> cancelled{};
        bool preparing{false};
    };

    struct ActiveLease {
        std::string model;
        int backend_slot{0};
    };

    bool waiter_is_next_locked(std::uint64_t id, time_point now) const;
    void erase_waiter_locked(std::uint64_t id);
    foundation::Result<int> issue_lease_locked(const std::string& name, int backend_slot);
    foundation::Result<int> backend_slot_for_lease_locked(
        const std::string& name, int lease_id) const;
    foundation::Result<void> prepare_capacity_for(const std::string& name);
    int estimated_vram_locked() const;
    int available_vram_locked() const;
    void select_primary_locked();

    mutable std::mutex mutex_;
    ModelRegistry& registry_;
    std::unordered_map<std::string, std::unique_ptr<IBackend>> instances_;
    std::optional<std::string> current_loaded_;
    int active_requests_{0};
    std::unordered_map<std::string, int> active_requests_by_model_;
    std::unordered_map<int, ActiveLease> active_leases_;
    std::int64_t next_lease_id_{1};
    std::unordered_set<std::string> draining_models_;
    std::unordered_set<std::string> resizing_models_;
    int vram_budget_mb_{0};
    int vram_safety_margin_mb_{1024};
    std::string last_resource_decision_{};
    std::condition_variable cv_;
    std::deque<SlotWaiter> waiters_;
    std::uint64_t next_waiter_id_{1};
    std::size_t max_queue_size_{128};
    std::recursive_mutex swap_mutex_;
    std::atomic<bool> swap_in_progress_{false};
    std::atomic<bool> swap_cancel_{false};
};

} // namespace inferdeck::model
