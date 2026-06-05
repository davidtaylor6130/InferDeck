#include "engine/slot_pool.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace inferdeck::engine {

SlotPool::SlotPool(int n_slots) {
    if (n_slots < 1) n_slots = 1;
    slots_.reserve(n_slots);
    for (int i = 0; i < n_slots; ++i) {
        slots_.emplace_back(i);
    }
}

int SlotPool::n_free() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int n = 0;
    for (const auto& s : slots_) if (!s.is_busy()) ++n;
    return n;
}

int SlotPool::n_busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int n = 0;
    for (const auto& s : slots_) if (s.is_busy()) ++n;
    return n;
}

Slot& SlotPool::slot(int id) {
    if (id < 0 || id >= static_cast<int>(slots_.size())) {
        throw std::out_of_range("SlotPool::slot: invalid id");
    }
    return slots_[id];
}

const Slot& SlotPool::slot(int id) const {
    if (id < 0 || id >= static_cast<int>(slots_.size())) {
        throw std::out_of_range("SlotPool::slot: invalid id");
    }
    return slots_[id];
}

std::optional<SlotMatch> SlotPool::best_lcp_match_locked(
    const TokenSequence& incoming) const {
    std::optional<SlotMatch> best;
    for (const auto& s : slots_) {
        if (s.is_busy()) continue;
        int lcp = s.prev_tokens().lcp_with(incoming);
        if (lcp == 0) continue;
        if (!best || lcp > best->prefix_tokens) {
            best = SlotMatch{s.id(), lcp, true};
        }
    }
    return best;
}

std::optional<SlotMatch> SlotPool::best_lcp_match(
    const TokenSequence& incoming) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return best_lcp_match_locked(incoming);
}

std::optional<int> SlotPool::find_free_slot_locked() const {
    for (const auto& s : slots_) {
        if (!s.is_busy()) return s.id();
    }
    return std::nullopt;
}

SlotMatch SlotPool::do_acquire_locked(const TokenSequence& incoming) {
    auto match = best_lcp_match_locked(incoming);
    if (match) {
        slots_[match->slot_id].mark_busy();
        return *match;
    }
    auto free_id = find_free_slot_locked();
    if (free_id) {
        slots_[*free_id].mark_busy();
        return SlotMatch{*free_id, 0, false};
    }
    return SlotMatch{-1, 0, false};
}

SlotMatch SlotPool::acquire_with_match(const TokenSequence& incoming) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto m = do_acquire_locked(incoming);
    if (m.slot_id >= 0) {
        cv_.notify_all();
    }
    return m;
}

std::pair<AcquireStatus, SlotMatch> SlotPool::acquire(
    const TokenSequence& incoming, const AcquireOptions& opts) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = clock::now() + opts.timeout;
    while (true) {
        if (cancelled_) {
            return {AcquireStatus::Cancelled, SlotMatch{}};
        }
        auto m = do_acquire_locked(incoming);
        if (m.slot_id >= 0) {
            cv_.notify_all();
            return {AcquireStatus::Acquired, m};
        }
        if (!opts.block) {
            return {AcquireStatus::Timeout, SlotMatch{}};
        }
        ++waiters_;
        cv_.wait_until(lock, deadline);
        --waiters_;
        if (clock::now() >= deadline) {
            return {AcquireStatus::Timeout, SlotMatch{}};
        }
    }
}

void SlotPool::release(int slot_id) {
    if (slot_id < 0 || slot_id >= static_cast<int>(slots_.size())) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_[slot_id].mark_idle();
    }
    cv_.notify_all();
}

void SlotPool::cancel_waiters() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
    }
    cv_.notify_all();
}

int SlotPool::waiter_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_;
}

} // namespace inferdeck::engine
