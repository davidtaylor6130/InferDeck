#include "model/backend_coordinator.hpp"
#include "foundation/logging.hpp"

#include <algorithm>
#include <cstdlib>
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
    const LifecycleControl control{deadline, cancelled};
    std::optional<int> recovery_slots;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = instances_.find(name);
        if (existing != instances_.end() && existing->second &&
            existing->second->is_loaded()) {
            if (existing->second->execution_healthy()) {
                if (is_primary_model(existing->second->info())) {
                    current_loaded_ = name;
                }
                return foundation::Ok();
            }
            recovery_slots = existing->second->n_slots();
        }
    }
    if (recovery_slots) {
        const auto unloaded = unload_with_control(name, control);
        if (!unloaded) return unloaded;
    }
    IBackend* instance = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = instances_.find(name);
        if (existing == instances_.end() || !existing->second) {
            auto backend = registry_.create_result(name);
            if (!backend) {
                return foundation::Err<void>(backend.error().code, backend.error().message);
            }
            instances_[name] = std::move(backend.value());
        }
        instance = instances_.at(name).get();
    }
    if (recovery_slots && !instance->info().concurrency_auto && instance->n_slots() != *recovery_slots) {
        const auto resized = instance->resize_slots(*recovery_slots, control);
        if (!resized) return resized;
    }
    auto r = instance->load(control);
    if (!r) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (instance->estimate_vram_mb(instance->n_slots()) > 0) {
                invalidate_vram_observation_locked();
                ++resource_generation_;
            }
        }
        cv_.notify_all();
        return r;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (is_primary_model(instance->info())) {
            current_loaded_ = name;
        }
        if (instance->estimate_vram_mb(instance->n_slots()) > 0) {
            invalidate_vram_observation_locked();
            ++resource_generation_;
        }
    }
    cv_.notify_all();
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
            const auto pins = validation_pins_.find(name);
            return (active != active_requests_by_model_.end() && active->second > 0) ||
                   (pins != validation_pins_.end() && pins->second > 0);
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

    int used_before_unload = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        used_before_unload = observed_vram_used_mb_;
    }
    auto r = instance ? instance->unload(control) : foundation::Ok();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!r && instance) {
            instances_[name] = std::move(instance);
            if (was_primary) current_loaded_ = name;
        }
        if (r) {
            active_requests_by_model_.erase(name);
            background_requests_by_model_.erase(name);
        }
        draining_models_.erase(name);
        if (r && released_vram > 0) {
            invalidate_vram_observation_locked();
            ++resource_generation_;
        }
    }
    cv_.notify_all();
    if (r && released_vram > 0) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (observed_vram_total_mb_ > 0 && used_before_unload > 0) {
            const int released_target = used_before_unload - released_vram / 2;
            const auto deadline = std::min(control.deadline, clock::now() + std::chrono::seconds{30});
            auto last_sample = observed_vram_at_;
            int last_used = -1;
            int stable_samples = 0;
            bool settled = false;
            while (!settled && !shutting_down_.load() && clock::now() < deadline) {
                cv_.wait_for(lock, std::chrono::milliseconds{250});
                if (!live_vram_observation_fresh_locked() || observed_vram_at_ == last_sample) continue;
                last_sample = observed_vram_at_;
                stable_samples = last_used >= 0 && std::abs(last_used - observed_vram_used_mb_) < 256
                    ? stable_samples + 1 : 0;
                settled = stable_samples >= 1 &&
                    (observed_vram_used_mb_ <= released_target || stable_samples >= 3);
                last_used = observed_vram_used_mb_;
            }
            foundation::LOG_INFO("gpu_release_settled", "model={} settled={} used_before_mb={} used_now_mb={}",
                                 name, settled, used_before_unload, observed_vram_used_mb_);
        }
    }
    return r;
}

foundation::Result<void> BackendCoordinator::ensure_loaded(const std::string& name) {
    if (is_ready(name)) return foundation::Ok();
    return load(name);
}

foundation::Result<void> BackendCoordinator::swap_to(const std::string& name) {
  return swap_to_with_control(name, {});
}

foundation::Result<void> BackendCoordinator::swap_to_with_control(
    const std::string& name, const LifecycleControl& control) {
  if (is_ready(name)) return foundation::Ok();
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
  if (is_ready(name)) return foundation::Ok();
  auto priority_allowed = require_priority_session_allows(name);
  if (!priority_allowed) return priority_allowed;
  if (is_loaded(name)) {
    return load_with_lock_deadline(name, control.deadline, control.cancelled);
  }
  if (!registry_.has_factory(info->runtime)) {
    return foundation::Err<void>(foundation::ErrorCode::Unavailable,
                                 "runtime not registered: " + info->runtime);
  }
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
  if (loaded.error().code == foundation::ErrorCode::OutOfMemory &&
      !control.is_cancelled() && !control.is_expired()) {
    (void)unload_with_control(name, control);
    auto reclaimed = prepare_capacity_for(name, control, true);
    if (reclaimed) {
      loaded = load_with_lock_deadline(name, control.deadline, control.cancelled);
      if (loaded) return loaded;
    } else if (reclaimed.error().code == foundation::ErrorCode::Cancelled ||
               reclaimed.error().code == foundation::ErrorCode::Timeout) {
      return reclaimed;
    }
  }

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

void BackendCoordinator::request_swap_cancel(const std::string& target) {
  std::lock_guard<std::mutex> lock(swap_tokens_mutex_);
  bool matched = false;
  for (auto& [name, token] : swap_tokens_) {
    if (target.empty() || name == target) {
      token->store(true);
      matched = true;
    }
  }
  if (matched) return;
  if (target.empty()) swap_cancel_.store(true);
  else pending_cancel_ = {target, clock::now() + std::chrono::seconds{5}};
}

foundation::Result<void> BackendCoordinator::swap_to_cancellable(
    const std::string& name, std::chrono::milliseconds timeout,
    std::function<bool()> cancelled) {
  auto token = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard<std::mutex> lock(swap_tokens_mutex_);
    bool cancelled_early = swap_cancel_.exchange(false);
    if (pending_cancel_) {
      cancelled_early = cancelled_early ||
          (pending_cancel_->first == name && clock::now() < pending_cancel_->second);
      if (cancelled_early || clock::now() >= pending_cancel_->second) pending_cancel_.reset();
    }
    if (cancelled_early) token->store(true);
    swap_tokens_.emplace_back(name, token);
  }
  struct TokenGuard {
    BackendCoordinator& self;
    std::shared_ptr<std::atomic<bool>> token;
    ~TokenGuard() {
      std::lock_guard<std::mutex> lock(self.swap_tokens_mutex_);
      std::erase_if(self.swap_tokens_, [&](const auto& e) { return e.second == token; });
    }
  } guard{*this, token};
  const LifecycleControl control{
      clock::now() + timeout,
      [this, token, cancelled = std::move(cancelled)] {
        return token->load() || shutting_down_.load() || (cancelled && cancelled());
      }};
  auto info = registry_.get_info_result(name);
  if (!info) return foundation::Err<void>(info.error().code, info.error().message);
  if (is_independent_sidecar(*info)) {
    if (token->load()) {
      return foundation::Err(foundation::ErrorCode::Cancelled,
                             "swap cancelled before sidecar load");
    }
    return load_with_lock_deadline(name, control.deadline, control.cancelled);
  }
  if (is_ready(name)) return foundation::Ok();
  if (token->load()) {
    return foundation::Err(foundation::ErrorCode::Cancelled, "swap cancelled before start");
  }
  swap_in_progress_.store(true);
  foundation::Result<void> result;
  try {
    result = swap_to_with_control(name, control);
  } catch (...) {
    result = foundation::Err(foundation::ErrorCode::Internal, "swap threw");
  }
  swap_in_progress_.store(false);
  return result;
}

}
