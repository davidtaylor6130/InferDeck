#include "model/backend_coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>

namespace inferdeck::model {

namespace {

bool is_primary_model(const ModelInfo& info) {
    return info.role == ModelRole::Conversation;
}

bool is_priority_media_model(const ModelInfo& info) {
    return info.role == ModelRole::Media &&
        (info.supports("audio_speech") ||
         info.supports("audio_transcription"));
}

bool is_independent_sidecar(const ModelInfo& info) {
    return !is_primary_model(info) && info.compute == ModelCompute::Cpu;
}

}
foundation::Result<int> BackendCoordinator::acquire_slot(
    const std::string& name, const AcquireSlotOptions& opts) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = clock::now() + opts.timeout;
    if (!opts.block) {
        if (draining_models_.contains(name)) {
            return foundation::Err<int>(foundation::ErrorCode::Unavailable,
                                         "model is draining: " + name);
        }
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second || !it->second->is_loaded()) {
            return foundation::Err<int>(foundation::ErrorCode::NotFound,
                                         "model not loaded: " + name);
        }
        if (!waiters_.empty() || resizing_models_.contains(name) ||
            request_waits_for_priority_media_locked(name, opts.reservation_key) ||
            !admission_pool_allows_locked(name)) {
            return foundation::Err<int>(foundation::ErrorCode::Unavailable,
                                         "request queue is busy: " + name);
        }
        auto slot = it->second->acquire_slot();
        if (!slot) return slot;
        auto lease = issue_lease_locked(name, *slot);
        if (!lease) (void)it->second->release_slot(*slot);
        return lease;
    }
    if (waiters_.size() >= max_queue_size_) {
        return foundation::Err<int>(foundation::ErrorCode::Unavailable,
                                     "request queue is full");
    }
    const std::uint64_t waiter_id = next_waiter_id_++;
    waiters_.push_back({waiter_id, name, std::clamp(opts.priority, -100, 100),
                        clock::now(), deadline, opts.cancelled,
                        opts.reservation_key, false, false,
                        std::nullopt});
    while (true) {
        const auto now = clock::now();
        if (opts.cancelled && opts.cancelled()) {
            erase_waiter_locked(waiter_id);
            cv_.notify_all();
            return foundation::Err<int>(foundation::ErrorCode::Cancelled,
                                         "request cancelled while queued: " + name);
        }
        if (now >= deadline) {
            erase_waiter_locked(waiter_id);
            cv_.notify_all();
            return foundation::Err<int>(foundation::ErrorCode::Timeout,
                                         "timeout waiting in request queue: " + name);
        }
        if (draining_models_.contains(name)) {
            cv_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds{100}));
            continue;
        }
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second || !it->second->is_loaded()) {
            if (opts.prepare && waiter_is_next_locked(waiter_id, now)) {
                auto waiter = std::find_if(waiters_.begin(), waiters_.end(),
                    [waiter_id](const SlotWaiter& item) { return item.id == waiter_id; });
                if (waiter != waiters_.end() && waiter->retry_after_generation &&
                    *waiter->retry_after_generation == resource_generation_) {
                    cv_.wait_until(lock, std::min(
                        deadline, now + std::chrono::milliseconds{100}));
                    continue;
                }
                if (waiter != waiters_.end() && !waiter->preparing) {
                    waiter->retry_after_generation.reset();
                    waiter->preparing = true;
                    waiter->prepared = false;
                    const auto generation_before = resource_generation_;
                    lock.unlock();
                    auto prepared = opts.prepare();
                    lock.lock();
                    waiter = std::find_if(waiters_.begin(), waiters_.end(),
                        [waiter_id](const SlotWaiter& item) { return item.id == waiter_id; });
                    if (waiter != waiters_.end()) waiter->preparing = false;
                    if (!prepared) {
                        if (prepared.error().code == foundation::ErrorCode::ResourceBusy) {
                            if (waiter != waiters_.end() &&
                                resource_generation_ == generation_before) {
                                waiter->retry_after_generation = generation_before;
                            }
                            continue;
                        }
                        erase_waiter_locked(waiter_id);
                        cv_.notify_all();
                        return foundation::Err<int>(prepared.error().code, prepared.error().message);
                    }
                    if (waiter != waiters_.end()) waiter->prepared = true;
                    continue;
                }
            }
            if (opts.prepare) {
                cv_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds{100}));
                continue;
            }
            erase_waiter_locked(waiter_id);
            cv_.notify_all();
            return foundation::Err<int>(foundation::ErrorCode::NotFound,
                                         "model not loaded: " + name);
        }
        if (!resizing_models_.contains(name) && waiter_is_next_locked(waiter_id, now)) {
            auto slot = it->second->acquire_slot();
            if (slot) {
                auto lease = issue_lease_locked(name, *slot);
                if (!lease) {
                    (void)it->second->release_slot(*slot);
                    erase_waiter_locked(waiter_id);
                    cv_.notify_all();
                    return lease;
                }
                erase_waiter_locked(waiter_id);
                cv_.notify_all();
                return lease;
            }
        }
        cv_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds{100}));
    }
}

foundation::Result<void> BackendCoordinator::release_slot(
    const std::string& name, int lease_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto lease = active_leases_.find(lease_id);
        if (lease == active_leases_.end()) {
            if (lease_id > 0 && lease_id < next_lease_id_) return foundation::Ok();
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                          "invalid slot lease");
        }
        if (lease->second.model != name) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                          "slot lease belongs to another model");
        }
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<void>(foundation::ErrorCode::NotFound,
                                          "model not loaded: " + name);
        }
        auto r = it->second->release_slot(lease->second.backend_slot);
        if (!r) return r;
        active_leases_.erase(lease);
        if (active_requests_ > 0) --active_requests_;
        auto active = active_requests_by_model_.find(name);
        if (active != active_requests_by_model_.end() && active->second > 0) --active->second;
        if (it->second->estimate_vram_mb(it->second->n_slots()) > 0) {
            ++resource_generation_;
        }
    }
    cv_.notify_all();
    return foundation::Ok();
}

void BackendCoordinator::set_max_queue_size(std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_queue_size_ = std::max<std::size_t>(1, size);
}

std::size_t BackendCoordinator::queued_request_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.size();
}

std::vector<QueueInfo> BackendCoordinator::queue() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = clock::now();
    std::vector<const SlotWaiter*> ordered;
    ordered.reserve(waiters_.size());
    for (const auto& waiter : waiters_) ordered.push_back(&waiter);
    std::sort(ordered.begin(), ordered.end(), [this, now](const auto* a, const auto* b) {
        const bool actionable_a = waiter_is_actionable_locked(*a);
        const bool actionable_b = waiter_is_actionable_locked(*b);
        if (actionable_a != actionable_b) return actionable_a;
        if (actionable_a && a->prepared != b->prepared) return a->prepared;
        const auto age_a = std::chrono::duration_cast<std::chrono::seconds>(now - a->enqueued).count();
        const auto age_b = std::chrono::duration_cast<std::chrono::seconds>(now - b->enqueued).count();
        const auto score_a = a->priority + age_a;
        const auto score_b = b->priority + age_b;
        return score_a == score_b ? a->id < b->id : score_a > score_b;
    });
    std::vector<QueueInfo> out;
    out.reserve(ordered.size());
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        const auto* waiter = ordered[i];
        out.push_back({waiter->id, waiter->model, waiter->priority, i + 1,
                       std::chrono::duration_cast<std::chrono::milliseconds>(now - waiter->enqueued).count(),
                       std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(waiter->deadline - now).count())});
    }
    return out;
}

std::uint64_t BackendCoordinator::reserve_priority_session(
    const std::string& key, const std::string& model,
    std::chrono::milliseconds duration) {
    if (key.empty() || model.empty() || duration <= std::chrono::milliseconds::zero()) return 0;
    std::uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = clock::now();
        std::erase_if(priority_sessions_, [now](const auto& entry) {
            return entry.second.active_holds == 0 && entry.second.deadline <= now;
        });
        token = next_priority_session_token_++;
        priority_sessions_[key] = PrioritySession{
            model, now + duration, token, 0};
    }
    cv_.notify_all();
    return token;
}

bool BackendCoordinator::refresh_priority_session(
    const std::string& key, std::uint64_t token,
    std::chrono::milliseconds duration) {
    bool refreshed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto session = priority_sessions_.find(key);
        if (session != priority_sessions_.end() &&
            session->second.token == token) {
            session->second.deadline = clock::now() + duration;
            refreshed = true;
        }
    }
    if (refreshed) cv_.notify_all();
    return refreshed;
}

std::optional<std::uint64_t> BackendCoordinator::hold_priority_session(
    const std::string& key, const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = priority_sessions_.find(key);
    if (session == priority_sessions_.end() ||
        session->second.model != model ||
        (session->second.active_holds == 0 &&
         session->second.deadline <= clock::now())) {
        return std::nullopt;
    }
    ++session->second.active_holds;
    return session->second.token;
}

void BackendCoordinator::complete_priority_session_hold(
    const std::string& key, std::uint64_t token,
    std::chrono::milliseconds duration) {
    bool completed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto session = priority_sessions_.find(key);
        if (session != priority_sessions_.end() &&
            session->second.token == token) {
            if (session->second.active_holds > 0) --session->second.active_holds;
            session->second.deadline = clock::now() + duration;
            completed = true;
        }
    }
    if (completed) cv_.notify_all();
}

void BackendCoordinator::release_priority_session(
    const std::string& key, std::uint64_t token) {
    bool released = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto session = priority_sessions_.find(key);
        if (session != priority_sessions_.end() &&
            session->second.token == token) {
            priority_sessions_.erase(session);
            released = true;
        }
    }
    if (released) cv_.notify_all();
}

bool BackendCoordinator::priority_session_matches(
    const std::string& key, const std::string& model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = priority_sessions_.find(key);
    return session != priority_sessions_.end() &&
        session->second.model == model &&
        (session->second.active_holds > 0 ||
         session->second.deadline > clock::now());
}

bool BackendCoordinator::priority_media_active_locked() const {
    for (const auto& [name, active] : active_requests_by_model_) {
        if (active <= 0) continue;
        const auto backend = instances_.find(name);
        if (backend != instances_.end() && backend->second &&
            is_priority_media_model(backend->second->info())) {
            return true;
        }
    }
    return false;
}

bool BackendCoordinator::model_is_primary_locked(const std::string& name) const {
    const auto backend = instances_.find(name);
    if (backend != instances_.end() && backend->second) {
        return is_primary_model(backend->second->info());
    }
    const auto info = registry_.get_info_result(name);
    return info && is_primary_model(*info);
}

bool BackendCoordinator::model_is_priority_media_locked(const std::string& name) const {
    const auto backend = instances_.find(name);
    if (backend != instances_.end() && backend->second) {
        return is_priority_media_model(backend->second->info());
    }
    const auto info = registry_.get_info_result(name);
    return info && is_priority_media_model(*info);
}

bool BackendCoordinator::model_is_independent_sidecar_locked(
    const std::string& name) const {
    const auto backend = instances_.find(name);
    if (backend != instances_.end() && backend->second) {
        return is_independent_sidecar(backend->second->info());
    }
    const auto info = registry_.get_info_result(name);
    return info && is_independent_sidecar(*info);
}

bool BackendCoordinator::admission_pool_allows_locked(
    const std::string& name) const {
    const auto find_info = [this](const std::string& model_name)
        -> std::optional<ModelInfo> {
        const auto backend = instances_.find(model_name);
        if (backend != instances_.end() && backend->second) {
            return backend->second->info();
        }
        const auto registered = registry_.get_info_result(model_name);
        if (registered) return *registered;
        return std::nullopt;
    };
    const auto target_info = find_info(name);
    if (!target_info) return false;
    int active = 0;
    for (const auto& [model_name, count] : active_requests_by_model_) {
        if (count <= 0) continue;
        const auto info = find_info(model_name);
        if (info && info->admission_pool == target_info->admission_pool) {
            active += count;
        }
    }
    return active < target_info->concurrency_limit;
}

bool BackendCoordinator::request_waits_for_priority_media_locked(
    const std::string& name, const std::string& reservation_key) const {
    const bool primary = model_is_primary_locked(name);
    if (primary) {
        bool any_session = false;
        bool matching_session = false;
        const auto now = clock::now();
        for (const auto& [key, session] : priority_sessions_) {
            if (session.active_holds == 0 && session.deadline <= now) continue;
            any_session = true;
            if (key == reservation_key && session.model == name) {
                matching_session = true;
            }
        }
        if (any_session && !matching_session) return true;
    }
    const bool media_reserved = priority_media_active_locked() ||
        std::any_of(waiters_.begin(), waiters_.end(), [this](const SlotWaiter& waiter) {
            return model_is_priority_media_locked(waiter.model);
        });
    if (!media_reserved) return false;
    return primary;
}

bool BackendCoordinator::waiter_is_actionable_locked(
    const SlotWaiter& waiter) const {
    if (waiter.preparing ||
        (waiter.retry_after_generation &&
         *waiter.retry_after_generation == resource_generation_)) {
        return false;
    }
    if (request_waits_for_priority_media_locked(
            waiter.model, waiter.reservation_key)) return false;
    if (!admission_pool_allows_locked(waiter.model)) return false;
    const auto backend = instances_.find(waiter.model);
    if (backend == instances_.end() || !backend->second ||
        !backend->second->is_loaded()) {
        if (model_is_independent_sidecar_locked(waiter.model)) return true;
        const bool another_prepare = std::any_of(
            waiters_.begin(), waiters_.end(), [this, &waiter](const SlotWaiter& other) {
                return other.id != waiter.id &&
                    (other.preparing ||
                     (other.prepared &&
                      !request_waits_for_priority_media_locked(
                          other.model, other.reservation_key)));
            });
        if (another_prepare) return false;
        return true;
    }
    return backend->second->n_free_slots() > 0 &&
        !draining_models_.contains(waiter.model) &&
        !resizing_models_.contains(waiter.model);
}

bool BackendCoordinator::waiter_is_next_locked(std::uint64_t id, time_point now) const {
    const bool has_prepared = std::any_of(
        waiters_.begin(), waiters_.end(), [this](const SlotWaiter& waiter) {
            return waiter.prepared && waiter_is_actionable_locked(waiter);
        });
    const SlotWaiter* selected = nullptr;
    std::int64_t selected_score = 0;
    for (const auto& waiter : waiters_) {
        if (!waiter_is_actionable_locked(waiter)) continue;
        if (has_prepared && !waiter.prepared &&
            !model_is_independent_sidecar_locked(waiter.model)) continue;
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - waiter.enqueued).count();
        const auto score = static_cast<std::int64_t>(waiter.priority) + age;
        if (!selected || score > selected_score || (score == selected_score && waiter.id < selected->id)) {
            selected = &waiter;
            selected_score = score;
        }
    }
    return selected && selected->id == id;
}

void BackendCoordinator::erase_waiter_locked(std::uint64_t id) {
    std::erase_if(waiters_, [id](const SlotWaiter& waiter) { return waiter.id == id; });
}

foundation::Result<int> BackendCoordinator::issue_lease_locked(
    const std::string& name, int backend_slot) {
    if (!admission_pool_allows_locked(name)) {
        return foundation::Err<int>(
            foundation::ErrorCode::ResourceBusy,
            "admission pool concurrency limit reached: " + name);
    }
    if (next_lease_id_ > std::numeric_limits<int>::max()) {
        return foundation::Err<int>(foundation::ErrorCode::Internal,
                                     "slot lease id space exhausted");
    }
    const auto lease_id = static_cast<int>(next_lease_id_++);
    active_leases_.emplace(lease_id, ActiveLease{name, backend_slot});
    ++active_requests_;
    ++active_requests_by_model_[name];
    return foundation::Ok(lease_id);
}

foundation::Result<int> BackendCoordinator::backend_slot_for_lease_locked(
    const std::string& name, int lease_id) const {
    const auto lease = active_leases_.find(lease_id);
    if (lease == active_leases_.end() || lease->second.model != name) {
        return foundation::Err<int>(foundation::ErrorCode::InvalidArgument,
                                     "invalid slot lease");
    }
    return foundation::Ok(lease->second.backend_slot);
}

}
