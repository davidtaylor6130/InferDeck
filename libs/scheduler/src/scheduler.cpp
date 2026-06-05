#include "scheduler/scheduler.hpp"

#include <utility>

namespace inferdeck::scheduler {

Scheduler::Scheduler(model::BackendCoordinator& coordinator)
    : coordinator_(coordinator) {}

engine::SlotPool& Scheduler::get_or_create_pool(
    const std::string& name, int n_slots) {
    auto it = pools_.find(name);
    if (it != pools_.end() && it->second) {
        return *it->second;
    }
    auto pool = std::make_unique<engine::SlotPool>(n_slots);
    auto& ref = *pool;
    pools_.emplace(name, std::move(pool));
    return ref;
}

foundation::Result<ScheduledSlot> Scheduler::acquire(
    const std::string& model_name,
    const engine::TokenSequence& token_seq,
    const ScheduleOptions& opts) {

    if (!coordinator_.registry().has(model_name)) {
        ++total_not_found_;
        return foundation::Err<ScheduledSlot>(
            foundation::ErrorCode::NotFound,
            "model not registered: " + model_name);
    }

    if (!coordinator_.is_loaded(model_name)) {
        ++total_unavailable_;
        return foundation::Err<ScheduledSlot>(
            foundation::ErrorCode::Unavailable,
            "model not loaded (use POST /v1/swap/to/" + model_name +
            " first): " + model_name);
    }

    const auto& info = coordinator_.registry().get_info(model_name);
    engine::SlotPool& pool = [&]() -> engine::SlotPool& {
        std::lock_guard<std::mutex> lock(mutex_);
        return get_or_create_pool(model_name, info.n_slots);
    }();

    engine::AcquireOptions pool_opts;
    pool_opts.block = opts.block;
    pool_opts.timeout = opts.timeout;

    auto [status, match] = pool.acquire(token_seq, pool_opts);
    if (status == engine::AcquireStatus::Timeout) {
        ++total_timeouts_;
        return foundation::Err<ScheduledSlot>(
            foundation::ErrorCode::Timeout,
            "timeout waiting for slot on: " + model_name);
    }
    if (status == engine::AcquireStatus::Cancelled) {
        ++total_timeouts_;
        return foundation::Err<ScheduledSlot>(
            foundation::ErrorCode::Cancelled,
            "scheduler cancelled: " + model_name);
    }

    ScheduledSlot out;
    out.model_name = model_name;
    out.slot_id = match.slot_id;
    out.prefix_tokens = match.prefix_tokens;
    out.lcp_hit = match.lcp_hit;
    out.raw_slot = &pool.slot(match.slot_id);
    out.sched_owner = this;

    if (opts.update_prev_tokens) {
        out.raw_slot->set_prev_tokens(token_seq);
    }

    if (match.lcp_hit) {
        ++total_lcp_hits_;
    } else {
        ++total_lcp_misses_;
    }
    ++total_acquired_;
    ++active_;

    return foundation::Ok(std::move(out));
}

foundation::Result<void> Scheduler::release(ScheduledSlot& slot) {
    if (!slot.valid()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "ScheduledSlot not owned by this scheduler");
    }
    engine::SlotPool* p = pool(slot.model_name);
    if (!p) {
        return foundation::Err<void>(
            foundation::ErrorCode::NotFound,
            "no pool for model: " + slot.model_name);
    }
    p->release(slot.slot_id);
    slot.raw_slot = nullptr;
    slot.sched_owner = nullptr;
    ++total_released_;
    --active_;
    return foundation::Ok();
}

void Scheduler::cancel_waiters() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, p] : pools_) {
        if (p) p->cancel_waiters();
    }
}

engine::SlotPool* Scheduler::pool(const std::string& model_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(model_name);
    if (it == pools_.end() || !it->second) return nullptr;
    return it->second.get();
}

} // namespace inferdeck::scheduler
