#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "engine/slot_pool.hpp"
#include "engine/token_sequence.hpp"
#include "foundation/result.hpp"
#include "model/backend_coordinator.hpp"

namespace inferdeck::scheduler {

class Scheduler;

struct ScheduledSlot {
    std::string model_name{};
    int slot_id{-1};
    int prefix_tokens{0};
    bool lcp_hit{false};

    void release() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return raw_slot != nullptr && sched_owner != nullptr;
    }

    engine::Slot* raw_slot{nullptr};
    Scheduler* sched_owner{nullptr};
};

struct ScheduleOptions {
    std::chrono::milliseconds timeout{30000};
    bool block{true};
    bool update_prev_tokens{true};
};

class Scheduler {
public:
    explicit Scheduler(model::BackendCoordinator& coordinator);

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    [[nodiscard]] foundation::Result<ScheduledSlot> acquire(
        const std::string& model_name,
        const engine::TokenSequence& token_seq,
        const ScheduleOptions& opts = {});

    [[nodiscard]] foundation::Result<void> release(ScheduledSlot& slot);

    void cancel_waiters();

    [[nodiscard]] engine::SlotPool* pool(const std::string& model_name);

    [[nodiscard]] int active_count() const noexcept { return active_.load(); }
    [[nodiscard]] int total_acquired() const noexcept { return total_acquired_.load(); }
    [[nodiscard]] int total_released() const noexcept { return total_released_.load(); }
    [[nodiscard]] int total_lcp_hits() const noexcept { return total_lcp_hits_.load(); }
    [[nodiscard]] int total_lcp_misses() const noexcept { return total_lcp_misses_.load(); }
    [[nodiscard]] int total_timeouts() const noexcept { return total_timeouts_.load(); }
    [[nodiscard]] int total_unavailable() const noexcept { return total_unavailable_.load(); }
    [[nodiscard]] int total_not_found() const noexcept { return total_not_found_.load(); }

private:
    engine::SlotPool& get_or_create_pool(const std::string& name, int n_slots);

    mutable std::mutex mutex_;
    model::BackendCoordinator& coordinator_;
    std::unordered_map<std::string, std::unique_ptr<engine::SlotPool>> pools_;

    std::atomic<int> active_{0};
    std::atomic<int> total_acquired_{0};
    std::atomic<int> total_released_{0};
    std::atomic<int> total_lcp_hits_{0};
    std::atomic<int> total_lcp_misses_{0};
    std::atomic<int> total_timeouts_{0};
    std::atomic<int> total_unavailable_{0};
    std::atomic<int> total_not_found_{0};
};

inline void ScheduledSlot::release() noexcept {
    if (sched_owner && raw_slot) {
        (void)sched_owner->release(*this);
    }
    raw_slot = nullptr;
    sched_owner = nullptr;
    slot_id = -1;
    prefix_tokens = 0;
    lcp_hit = false;
    model_name.clear();
}

} // namespace inferdeck::scheduler
