#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "foundation/result.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"

namespace inferdeck::model {

struct AcquireSlotOptions {
    std::chrono::milliseconds timeout{30000};
    bool block{true};
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
    [[nodiscard]] int get_vram_usage() const;
    [[nodiscard]] const IModel* get_model(const std::string& name) const;

    [[nodiscard]] ModelRegistry& registry() noexcept { return registry_; }

    foundation::Result<int> acquire_slot(
        const std::string& name, const AcquireSlotOptions& opts = {});
    foundation::Result<void> release_slot(
        const std::string& name, int slot_id);

    foundation::Result<InferenceResult> predict(
        const std::string& name, int slot_id, const InferenceRequest& req);

    foundation::Result<InferenceResult> predict_stream(
        const std::string& name, int slot_id, const InferenceRequest& req,
        const IModel::TokenCallback& callback);

    void drain_active(std::chrono::milliseconds timeout = std::chrono::milliseconds{30000});
    int active_request_count() const;

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
        int requested_slot{-1};
        time_point deadline{};
    };

    void notify_waiters_locked(const std::string& name);

    mutable std::mutex mutex_;
    ModelRegistry& registry_;
    std::unordered_map<std::string, std::unique_ptr<IModel>> instances_;
    std::optional<std::string> current_loaded_;
    int active_requests_{0};
    std::condition_variable cv_;
    std::atomic<bool> swap_in_progress_{false};
    std::atomic<bool> swap_cancel_{false};
};

} // namespace inferdeck::model
