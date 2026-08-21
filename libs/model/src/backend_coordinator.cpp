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

BackendCoordinator::BackendCoordinator(ModelRegistry& registry)
    : registry_(registry) {}

foundation::Result<void> BackendCoordinator::register_existing(const ModelInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        registry_.register_model(info);
    } catch (const std::exception& e) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument, e.what());
    }
    return foundation::Ok();
}

foundation::Result<void> BackendCoordinator::unregister(const std::string& name) {
    const auto info = registry_.get_info_result(name);
    std::unique_lock<std::recursive_mutex> swap_lock(swap_mutex_, std::defer_lock);
    std::unique_lock<std::recursive_mutex> sidecar_lock(sidecar_mutex_, std::defer_lock);
    if (info && is_independent_sidecar(*info)) sidecar_lock.lock();
    else swap_lock.lock();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it != instances_.end() && it->second && it->second->is_loaded()) {
            return foundation::Err<void>(foundation::ErrorCode::AlreadyExists,
                                          "cannot unregister loaded model: " + name);
        }
        instances_.erase(name);
    }
    try {
        registry_.unregister_model(name);
    } catch (const std::exception& error) {
        return foundation::Err<void>(foundation::ErrorCode::AlreadyExists,
                                     error.what());
    }
    return foundation::Ok();
}

foundation::Result<void> BackendCoordinator::load(const std::string& name) {
    return load_with_lock_deadline(name, clock::time_point::max(), {});
}

foundation::Result<void> BackendCoordinator::load_with_lock_deadline(
    const std::string& name, time_point deadline,
    const std::function<bool()>& cancelled) {
    const auto info = registry_.get_info_result(name);
    if (!info) return foundation::Err<void>(info.error().code, info.error().message);
    std::unique_lock<std::recursive_mutex> swap_lock(swap_mutex_, std::defer_lock);
    std::unique_lock<std::recursive_mutex> sidecar_lock(sidecar_mutex_, std::defer_lock);
    auto* lifecycle_lock = is_independent_sidecar(*info) ? &sidecar_lock : &swap_lock;
    while (true) {
        if (cancelled && cancelled()) {
            return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                          "model load cancelled: " + name);
        }
        if (clock::now() >= deadline) {
            return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                          "timeout waiting to start model load: " + name);
        }
        if (lifecycle_lock->try_lock()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    IBackend* instance = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = instances_.find(name);
        if (existing != instances_.end() && existing->second &&
            existing->second->is_loaded()) {
            if (is_primary_model(existing->second->info())) {
                current_loaded_ = name;
            }
            return foundation::Ok();
        }
        if (existing == instances_.end() || !existing->second) {
            auto backend = registry_.create_result(name);
            if (!backend) {
                return foundation::Err<void>(backend.error().code, backend.error().message);
            }
            instances_[name] = std::move(backend.value());
        }
        instance = instances_.at(name).get();
    }
    const LifecycleControl control{deadline, cancelled};
    auto r = instance->load(control);
    if (!r) return r;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (is_primary_model(instance->info())) {
            current_loaded_ = name;
        }
        if (instance->estimate_vram_mb(instance->n_slots()) > 0) {
            ++resource_generation_;
        }
    }
    cv_.notify_all();
    (void)instance->reset_all_slots();
    return foundation::Ok();
}

foundation::Result<void> BackendCoordinator::unload_current() {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!current_loaded_.has_value()) {
            return foundation::Ok();
        }
        name = *current_loaded_;
    }
    return unload(name);
}

foundation::Result<void> BackendCoordinator::unload(const std::string& name) {
    return unload_with_control(name, {});
}

foundation::Result<void> BackendCoordinator::unload_with_control(
    const std::string& name, const LifecycleControl& control) {
    const auto info = registry_.get_info_result(name);
    std::unique_lock<std::recursive_mutex> swap_lock(swap_mutex_, std::defer_lock);
    std::unique_lock<std::recursive_mutex> sidecar_lock(sidecar_mutex_, std::defer_lock);
    auto* lifecycle_lock = info && is_independent_sidecar(*info)
        ? &sidecar_lock : &swap_lock;
    while (!lifecycle_lock->try_lock()) {
        if (control.is_cancelled()) {
            return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                         "model unload cancelled: " + name);
        }
        if (control.is_expired()) {
            return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                         "timeout waiting to start model unload: " + name);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    const auto drain_deadline = std::min(
        control.deadline, clock::now() + std::chrono::milliseconds{30000});
    std::unique_ptr<IBackend> instance;
    bool was_primary = false;
    int released_vram = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        draining_models_.insert(name);
        const auto has_active_requests = [&] {
            const auto active = active_requests_by_model_.find(name);
            return active != active_requests_by_model_.end() && active->second > 0;
        };
        while (has_active_requests() && clock::now() < drain_deadline &&
               !control.is_cancelled()) {
            cv_.wait_for(lock, std::chrono::milliseconds{100});
        }
        if (control.is_cancelled()) {
            draining_models_.erase(name);
            lock.unlock();
            cv_.notify_all();
            return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                          "model unload cancelled: " + name);
        }
        if (has_active_requests()) {
            draining_models_.erase(name);
            lock.unlock();
            cv_.notify_all();
            return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                          "timeout draining active requests for: " + name);
        }
        auto it = instances_.find(name);
        if (it != instances_.end() && it->second) {
            released_vram = it->second->estimate_vram_mb(it->second->n_slots());
            instance = std::move(it->second);
            instances_.erase(it);
        }
        was_primary = current_loaded_.has_value() && *current_loaded_ == name;
        if (was_primary) {
            current_loaded_.reset();
            select_primary_locked();
        }
    }

    auto r = instance ? instance->unload(control) : foundation::Ok();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!r && instance) {
            instances_[name] = std::move(instance);
            if (was_primary) current_loaded_ = name;
        }
        if (r) active_requests_by_model_.erase(name);
        draining_models_.erase(name);
        if (r && released_vram > 0) ++resource_generation_;
    }
    cv_.notify_all();
    return r;
}

foundation::Result<void> BackendCoordinator::ensure_loaded(const std::string& name) {
    if (is_loaded(name)) return foundation::Ok();
    return load(name);
}

foundation::Result<void> BackendCoordinator::swap_to(const std::string& name) {
  return swap_to_with_control(name, {});
}

foundation::Result<void> BackendCoordinator::swap_to_with_control(
    const std::string& name, const LifecycleControl& control) {
  if (is_loaded(name)) return foundation::Ok();
  auto info = registry_.get_info_result(name);
  if (!info) return foundation::Err<void>(info.error().code, info.error().message);
  if (is_independent_sidecar(*info)) {
    return load_with_lock_deadline(name, control.deadline, control.cancelled);
  }
  std::unique_lock<std::recursive_mutex> swap_lock(swap_mutex_, std::defer_lock);
  while (!swap_lock.try_lock()) {
    if (control.is_cancelled()) {
      return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                   "model swap cancelled: " + name);
    }
    if (control.is_expired()) {
      return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                   "timeout waiting to start model swap: " + name);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  if (is_loaded(name)) return foundation::Ok();
  auto priority_allowed = require_priority_session_allows(name);
  if (!priority_allowed) return priority_allowed;
  if (vram_budget_mb() <= 0) {
    auto current = get_loaded_model();
    auto drain_r = current ? unload_with_control(*current, control)
                           : foundation::Ok();
    if (!drain_r) return drain_r;
    auto loaded = load_with_lock_deadline(
        name, control.deadline, control.cancelled);
    if (loaded || !current) return loaded;
    const LifecycleControl recovery{
        clock::now() + std::chrono::seconds{30}, {}};
    auto restored = load_with_lock_deadline(
        *current, recovery.deadline, recovery.cancelled);
    if (restored) {
      std::lock_guard<std::mutex> lock(mutex_);
      current_loaded_ = *current;
      last_resource_decision_ =
          "load failed for " + name + "; previous residency restored";
    }
    return loaded;
  }
  const auto before = residency();
  const auto previous_primary = get_loaded_model();
  auto capacity = prepare_capacity_for(name, control);
  if (!capacity) return capacity;
  priority_allowed = require_priority_session_allows(name);
  if (!priority_allowed) return priority_allowed;
  auto loaded = load_with_lock_deadline(name, control.deadline, control.cancelled);
  if (loaded) return loaded;

  const LifecycleControl recovery{
      clock::now() + std::chrono::seconds{30}, {}};
  (void)unload_with_control(name, recovery);
  for (const auto& prior : before) {
    const auto* backend = get_backend(prior.name);
    if (backend && backend->is_loaded() && backend->n_slots() == prior.slots) continue;
    if (backend && backend->is_loaded()) {
      (void)unload_with_control(prior.name, recovery);
    }
    (void)load_with_lock_deadline(
        prior.name, recovery.deadline, recovery.cancelled);
  }
  if (previous_primary && is_loaded(*previous_primary)) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_loaded_ = *previous_primary;
    last_resource_decision_ = "load failed for " + name + "; previous residency restored";
  }
  return loaded;
}

foundation::Result<void> BackendCoordinator::swap_to_cancellable(
    const std::string& name, std::chrono::milliseconds timeout,
    std::function<bool()> cancelled) {
  const LifecycleControl control{
      clock::now() + timeout,
      [this, cancelled = std::move(cancelled)] {
        return swap_cancel_.load() || (cancelled && cancelled());
      }};
  auto info = registry_.get_info_result(name);
  if (!info) return foundation::Err<void>(info.error().code, info.error().message);
  if (is_independent_sidecar(*info)) {
    if (swap_cancel_.load()) {
      reset_swap_cancel();
      return foundation::Err(foundation::ErrorCode::Cancelled,
                             "swap cancelled before sidecar load");
    }
    auto result = load_with_lock_deadline(
        name, control.deadline, control.cancelled);
    reset_swap_cancel();
    return result;
  }
  if (is_loaded(name)) return foundation::Ok();
  if (swap_cancel_.load()) {
    reset_swap_cancel();
    return foundation::Err(foundation::ErrorCode::Cancelled, "swap cancelled before start");
  }
  swap_in_progress_.store(true);
  foundation::Result<void> result;
  try {
    if (swap_cancel_.load()) {
      result = foundation::Err(foundation::ErrorCode::Cancelled, "swap cancelled before load");
    } else {
      result = swap_to_with_control(name, control);
    }
  } catch (...) {
    result = foundation::Err(foundation::ErrorCode::Internal, "swap threw");
  }
  swap_in_progress_.store(false);
  reset_swap_cancel();
  return result;
}

bool BackendCoordinator::is_loaded(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(name);
    if (it == instances_.end() || !it->second) return false;
    return it->second->is_loaded();
}

std::optional<std::string> BackendCoordinator::get_loaded_model() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_loaded_;
}

std::optional<std::string> BackendCoordinator::selected_model() const {
    return get_loaded_model();
}

ModelIdentitySnapshot BackendCoordinator::identity_snapshot(
    std::string requested, std::string resolved) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ModelIdentitySnapshot snapshot;
    snapshot.requested = std::move(requested);
    snapshot.resolved = std::move(resolved);
    snapshot.selected = current_loaded_;
    for (const auto& [name, backend] : instances_) {
        if (backend && backend->is_loaded()) snapshot.resident.push_back(name);
    }
    for (const auto& [name, active] : active_requests_by_model_) {
        if (active > 0) snapshot.executing.push_back(name);
    }
    std::sort(snapshot.resident.begin(), snapshot.resident.end());
    std::sort(snapshot.executing.begin(), snapshot.executing.end());
    return snapshot;
}

std::vector<std::string> BackendCoordinator::get_loaded_models() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> loaded;
    for (const auto& [name, backend] : instances_) {
        if (backend && backend->is_loaded()) loaded.push_back(name);
    }
    std::sort(loaded.begin(), loaded.end());
    return loaded;
}

std::vector<ResidencyInfo> BackendCoordinator::residency() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ResidencyInfo> out;
    for (const auto& [name, backend] : instances_) {
        if (!backend || !backend->is_loaded()) continue;
        const auto active = active_requests_by_model_.find(name);
        const auto& info = backend->info();
        out.push_back({name, info.runtime, info.modality,
                       to_string(info.role), to_string(info.compute),
                       to_string(info.residency), info.admission_pool,
                       info.concurrency_limit, info.memory_required_mb,
                       info.eviction_eligible,
                       backend->n_slots(), backend->n_free_slots(),
                       active == active_requests_by_model_.end() ? 0 : active->second,
                       backend->estimate_vram_mb(backend->n_slots()),
                       current_loaded_ && *current_loaded_ == name,
                       resizing_models_.contains(name)});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

int BackendCoordinator::get_vram_usage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int total = 0;
    for (const auto& [_, m] : instances_) {
        if (m && m->is_loaded()) {
            total += m->vram_usage_mb();
        }
    }
    return total;
}

void BackendCoordinator::set_vram_budget(int total_mb, int safety_margin_mb) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int budget = std::max(0, total_mb);
        const int margin = std::max(0, safety_margin_mb);
        changed = budget != vram_budget_mb_ || margin != vram_safety_margin_mb_;
        if (changed) {
            vram_budget_mb_ = budget;
            vram_safety_margin_mb_ = margin;
            ++resource_generation_;
        }
    }
    if (changed) cv_.notify_all();
}

int BackendCoordinator::vram_budget_mb() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vram_budget_mb_;
}

int BackendCoordinator::vram_available_mb() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_vram_locked();
}

std::string BackendCoordinator::last_resource_decision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_resource_decision_;
}

const IBackend* BackendCoordinator::get_backend(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(name);
    if (it == instances_.end() || !it->second) return nullptr;
    return it->second.get();
}

const IModel* BackendCoordinator::get_model(const std::string& name) const {
    return dynamic_cast<const IModel*>(get_backend(name));
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

foundation::Result<InferenceResult> BackendCoordinator::predict(
    const std::string& name, int lease_id, const InferenceRequest& req) {
    IModel* inst = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<InferenceResult>(foundation::ErrorCode::NotFound,
                                                      "model not loaded: " + name);
        }
        inst = dynamic_cast<IModel*>(it->second.get());
        if (!inst || !it->second->info().supports("chat_completions")) {
            return foundation::Err<InferenceResult>(foundation::ErrorCode::InvalidArgument,
                                                      "backend does not support text generation: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<InferenceResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    // Safe to call unlocked: the caller holds a slot, so unload() drains before
    // the instance can be destroyed.
    return inst->predict(backend_slot, req);
}

foundation::Result<InferenceResult> BackendCoordinator::predict_stream(
    const std::string& name, int lease_id, const InferenceRequest& req,
    const IModel::TokenCallback& callback, const std::atomic<bool>* cancel) {
    IModel* inst = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<InferenceResult>(foundation::ErrorCode::NotFound,
                                                      "model not loaded: " + name);
        }
        inst = dynamic_cast<IModel*>(it->second.get());
        if (!inst || !it->second->info().supports("chat_completions")) {
            return foundation::Err<InferenceResult>(foundation::ErrorCode::InvalidArgument,
                                                      "backend does not support text generation: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<InferenceResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    return inst->predict_stream(backend_slot, req, callback, cancel);
}

foundation::Result<EmbeddingResult> BackendCoordinator::embed(
    const std::string& name, int lease_id, const EmbeddingRequest& request,
    const std::function<bool()>& cancelled) {
    IEmbeddingBackend* backend = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<EmbeddingResult>(foundation::ErrorCode::NotFound,
                                                      "model not loaded: " + name);
        }
        backend = dynamic_cast<IEmbeddingBackend*>(it->second.get());
        if (!backend || !it->second->info().supports("embeddings")) {
            return foundation::Err<EmbeddingResult>(foundation::ErrorCode::InvalidArgument,
                                                      "backend does not support embeddings: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<EmbeddingResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    return backend->embed(backend_slot, request, cancelled);
}

foundation::Result<ImageGenerationResult> BackendCoordinator::generate_images(
    const std::string& name, int lease_id, const ImageGenerationRequest& request,
    const std::function<bool(int)>& progress) {
    IImageBackend* backend = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<ImageGenerationResult>(foundation::ErrorCode::NotFound,
                                                           "model not loaded: " + name);
        }
        backend = dynamic_cast<IImageBackend*>(it->second.get());
        if (!backend || !it->second->info().supports("image_generation")) {
            return foundation::Err<ImageGenerationResult>(foundation::ErrorCode::InvalidArgument,
                                                           "backend does not support image generation: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<ImageGenerationResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    return backend->generate_images(backend_slot, request, progress);
}

foundation::Result<AudioResult> BackendCoordinator::synthesize(
    const std::string& name, int lease_id, const SpeechRequest& request,
    const std::function<bool(const std::byte*, std::size_t)>& stream) {
    ISpeechBackend* backend = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<AudioResult>(foundation::ErrorCode::NotFound,
                                                 "model not loaded: " + name);
        }
        backend = dynamic_cast<ISpeechBackend*>(it->second.get());
        if (!backend || !it->second->info().supports("audio_speech")) {
            return foundation::Err<AudioResult>(foundation::ErrorCode::InvalidArgument,
                                                 "backend does not support speech synthesis: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<AudioResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    return backend->synthesize(backend_slot, request, stream);
}

foundation::Result<void> BackendCoordinator::validate_speech_request(
    const std::string& name, const SpeechRequest& request) {
    auto created = registry_.create_result(name);
    if (!created) {
        return foundation::Err<void>(created.error().code,
                                     created.error().message);
    }
    auto* backend = dynamic_cast<ISpeechBackend*>(created->get());
    if (!backend || !(*created)->info().supports("audio_speech")) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "backend does not support speech synthesis: " + name);
    }
    return backend->validate_speech_request(request);
}

foundation::Result<TranscriptionResult> BackendCoordinator::transcribe(
    const std::string& name, int lease_id, const TranscriptionRequest& request,
    const std::function<bool(int)>& progress) {
    ITranscriptionBackend* backend = nullptr;
    int backend_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<TranscriptionResult>(foundation::ErrorCode::NotFound,
                                                         "model not loaded: " + name);
        }
        backend = dynamic_cast<ITranscriptionBackend*>(it->second.get());
        if (!backend || !it->second->info().supports("audio_transcription")) {
            return foundation::Err<TranscriptionResult>(foundation::ErrorCode::InvalidArgument,
                                                         "backend does not support transcription: " + name);
        }
        auto slot = backend_slot_for_lease_locked(name, lease_id);
        if (!slot) {
            return foundation::Err<TranscriptionResult>(slot.error().code, slot.error().message);
        }
        backend_slot = *slot;
    }
    return backend->transcribe(backend_slot, request, progress);
}

void BackendCoordinator::drain_active(std::chrono::milliseconds timeout) {
    auto deadline = clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    while (active_requests_ > 0 && clock::now() < deadline) {
        cv_.wait_for(lock, std::chrono::milliseconds{50});
    }
}

int BackendCoordinator::active_request_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_requests_;
}

int BackendCoordinator::active_request_count(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto active = active_requests_by_model_.find(name);
    return active == active_requests_by_model_.end() ? 0 : active->second;
}

int BackendCoordinator::estimated_vram_locked() const {
    int total = 0;
    for (const auto& [_, backend] : instances_) {
        if (backend && backend->is_loaded()) {
            total += backend->estimate_vram_mb(backend->n_slots());
        }
    }
    return total;
}

int BackendCoordinator::available_vram_locked() const {
    if (vram_budget_mb_ <= 0) return 0;
    return std::max(0, vram_budget_mb_ - vram_safety_margin_mb_ - estimated_vram_locked());
}

void BackendCoordinator::select_primary_locked() {
    current_loaded_.reset();
    int selected_vram = -1;
    for (const auto& [name, backend] : instances_) {
        if (backend && backend->is_loaded() && is_primary_model(backend->info())) {
            const int vram = backend->estimate_vram_mb(backend->n_slots());
            if (vram < selected_vram ||
                (vram == selected_vram && current_loaded_ && name > *current_loaded_)) {
                continue;
            }
            current_loaded_ = name;
            selected_vram = vram;
        }
    }
}

foundation::Result<void> BackendCoordinator::prepare_capacity_for(
    const std::string& name, const LifecycleControl& control) {
    if (control.is_cancelled()) {
        return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                     "capacity preparation cancelled: " + name);
    }
    if (control.is_expired()) {
        return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                     "capacity preparation timed out: " + name);
    }
    auto priority_allowed = require_priority_session_allows(name);
    if (!priority_allowed) return priority_allowed;
    auto info = registry_.get_info_result(name);
    if (!info) return foundation::Err<void>(info.error().code, info.error().message);
    const int required = info->vram_required_mb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = instances_.find(name);
        if (existing != instances_.end() && existing->second && existing->second->is_loaded()) {
            last_resource_decision_ = name + " already resident";
            return foundation::Ok();
        }
        if (vram_budget_mb_ <= 0 || available_vram_locked() >= required) {
            last_resource_decision_ = name + " fits without rebalancing";
            return foundation::Ok();
        }
    }

    while (true) {
        if (control.is_cancelled()) {
            return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                         "capacity resize cancelled: " + name);
        }
        if (control.is_expired()) {
            return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                         "capacity resize timed out: " + name);
        }
        std::string candidate;
        int next_slots = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (available_vram_locked() >= required) return foundation::Ok();
            for (const auto& [loaded_name, backend] : instances_) {
                const auto active = active_requests_by_model_.find(loaded_name);
                if (loaded_name == name || !backend || !backend->is_loaded() ||
                    (active != active_requests_by_model_.end() && active->second > 0) ||
                    backend->estimate_vram_mb(backend->n_slots()) <= 0 ||
                    backend->info().residency == ResidencyPolicy::Always ||
                    !backend->can_resize_slots() || backend->n_slots() <= backend->min_slots()) continue;
                candidate = loaded_name;
                next_slots = backend->n_slots() - 1;
                resizing_models_.insert(candidate);
                break;
            }
        }
        if (candidate.empty()) break;
        IBackend* backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            backend = instances_.at(candidate).get();
        }
        priority_allowed = require_priority_session_allows(name);
        if (!priority_allowed) {
            std::lock_guard<std::mutex> lock(mutex_);
            resizing_models_.erase(candidate);
            return priority_allowed;
        }
        auto resized = backend->resize_slots(next_slots, control);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            resizing_models_.erase(candidate);
            if (resized) {
                last_resource_decision_ = "shrunk " + candidate + " to " +
                    std::to_string(next_slots) + " slots for " + name;
                ++resource_generation_;
            }
        }
        cv_.notify_all();
        if (!resized) break;
    }

    while (true) {
        if (control.is_cancelled()) {
            return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                         "capacity eviction cancelled: " + name);
        }
        if (control.is_expired()) {
            return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                         "capacity eviction timed out: " + name);
        }
        std::string candidate;
        int candidate_vram = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (available_vram_locked() >= required) return foundation::Ok();
            for (const auto& [loaded_name, backend] : instances_) {
                const auto active = active_requests_by_model_.find(loaded_name);
                const int estimated = backend && backend->is_loaded()
                    ? backend->estimate_vram_mb(backend->n_slots()) : 0;
                if (loaded_name != name && backend && backend->is_loaded() &&
                    estimated > candidate_vram &&
                    backend->info().eviction_eligible &&
                    backend->info().residency != ResidencyPolicy::Always &&
                    (active == active_requests_by_model_.end() || active->second == 0)) {
                    candidate = loaded_name;
                    candidate_vram = estimated;
                }
            }
        }
        if (candidate.empty()) break;
        priority_allowed = require_priority_session_allows(name);
        if (!priority_allowed) return priority_allowed;
        auto unloaded = unload_with_control(candidate, control);
        if (!unloaded) return unloaded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_resource_decision_ = "evicted idle " + candidate + " for " + name;
        }
    }

    int available = 0;
    int maximum_available = 0;
    bool blocked_by_active = false;
    std::string blockers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        available = available_vram_locked();
        for (const auto& [loaded_name, backend] : instances_) {
            const auto active = active_requests_by_model_.find(loaded_name);
            if (!backend || !backend->is_loaded() ||
                backend->estimate_vram_mb(backend->n_slots()) <= 0 ||
                active == active_requests_by_model_.end() || active->second <= 0) {
                continue;
            }
            if (!blockers.empty()) blockers += ",";
            blockers += loaded_name;
            blocked_by_active = true;
        }
        maximum_available = std::max(0, vram_budget_mb_ - vram_safety_margin_mb_);
        if (blocked_by_active && required <= maximum_available) {
            last_resource_decision_ = "waiting for active residency before loading " + name;
        } else {
            last_resource_decision_ = "insufficient VRAM for " + name;
        }
    }
    if (blocked_by_active && required <= maximum_available) {
        return foundation::Err<void>(foundation::ErrorCode::ResourceBusy,
            "VRAM capacity for " + name + " is temporarily held by active model(s): " + blockers);
    }
    return foundation::Err<void>(foundation::ErrorCode::OutOfMemory,
        "insufficient VRAM for " + name + ": required_mb=" + std::to_string(required) +
        " available_mb=" + std::to_string(available));
}

foundation::Result<void> BackendCoordinator::require_priority_session_allows(
    const std::string& name) {
    bool blocked = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = clock::now();
        for (const auto& [_, session] : priority_sessions_) {
            if (session.active_holds == 0 && session.deadline <= now) continue;
            if (session.model != name) {
                blocked = true;
                break;
            }
        }
        if (blocked) {
            ++resource_generation_;
            last_resource_decision_ =
                "voice session reserved before loading " + name;
        }
    }
    if (!blocked) return foundation::Ok();
    cv_.notify_all();
    return foundation::Err<void>(
        foundation::ErrorCode::ResourceBusy,
        "voice session has priority before loading model: " + name);
}

} // namespace inferdeck::model
