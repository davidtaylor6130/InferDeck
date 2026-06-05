#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <vector>

#include "engine/slot.hpp"

namespace inferdeck::engine {

struct SlotMatch {
    int slot_id{-1};
    int prefix_tokens{0};
    bool lcp_hit{false};
};

struct AcquireOptions {
    bool block{true};
    std::chrono::milliseconds timeout{30000};
};

enum class AcquireStatus {
    Acquired,
    Timeout,
    Cancelled,
};

class SlotPool {
public:
    explicit SlotPool(int n_slots = 2);

    [[nodiscard]] int n_slots() const noexcept { return static_cast<int>(slots_.size()); }
    [[nodiscard]] int n_free() const;
    [[nodiscard]] int n_busy() const;

    [[nodiscard]] Slot& slot(int id);
    [[nodiscard]] const Slot& slot(int id) const;

    [[nodiscard]] std::optional<SlotMatch> best_lcp_match(
        const TokenSequence& incoming) const;

    [[nodiscard]] SlotMatch acquire_with_match(
        const TokenSequence& incoming);

    [[nodiscard]] std::pair<AcquireStatus, SlotMatch> acquire(
        const TokenSequence& incoming, const AcquireOptions& opts = {});

    void release(int slot_id);

    void cancel_waiters();
    int waiter_count() const;

private:
    using clock = std::chrono::steady_clock;

    std::optional<int> find_free_slot_locked() const;
    std::optional<SlotMatch> best_lcp_match_locked(
        const TokenSequence& incoming) const;
    SlotMatch do_acquire_locked(const TokenSequence& incoming);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Slot> slots_;
    bool cancelled_{false};
    int waiters_{0};
};

} // namespace inferdeck::engine
