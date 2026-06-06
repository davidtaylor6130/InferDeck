#include "model/backend_coordinator.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace inferdeck::model {

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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it != instances_.end() && it->second && it->second->is_loaded()) {
            return foundation::Err<void>(foundation::ErrorCode::AlreadyExists,
                                          "cannot unregister loaded model: " + name);
        }
        instances_.erase(name);
    }
    registry_.unregister_model(name);
    return foundation::Ok();
}

foundation::Result<void> BackendCoordinator::load(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_loaded_.has_value() && *current_loaded_ == name) {
            auto it = instances_.find(name);
            if (it != instances_.end() && it->second && it->second->is_loaded()) {
                return foundation::Ok();
            }
        }
        auto existing = instances_.find(name);
        if (existing == instances_.end() || !existing->second) {
            auto model = registry_.create(name);
            if (!model) {
                return foundation::Err<void>(foundation::ErrorCode::NotFound,
                                              "model not registered: " + name);
            }
            instances_[name] = std::move(model);
        }
    }
    auto& inst = instances_[name];
    auto r = inst->load();
    if (!r) return r;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_loaded_ = name;
    }
    cv_.notify_all();
    (void)inst->reset_all_slots();
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
    auto drain_deadline = clock::now() + std::chrono::milliseconds{30000};
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (active_requests_ > 0 && clock::now() < drain_deadline) {
            cv_.wait_for(lock, std::chrono::milliseconds{100});
        }
        if (active_requests_ > 0) {
            return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                          "timeout draining active requests for: " + name);
        }
    }
    foundation::Result<void> r = foundation::Ok();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it != instances_.end() && it->second) {
            r = it->second->unload();
        }
        if (current_loaded_.has_value() && *current_loaded_ == name) {
            current_loaded_.reset();
        }
    }
    cv_.notify_all();
    return r;
}

foundation::Result<void> BackendCoordinator::ensure_loaded(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_loaded_.has_value() && *current_loaded_ == name) {
            auto it = instances_.find(name);
            if (it != instances_.end() && it->second && it->second->is_loaded()) {
                return foundation::Ok();
            }
        }
    }
    return load(name);
}

foundation::Result<void> BackendCoordinator::swap_to(const std::string& name) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_loaded_.has_value() && *current_loaded_ == name) {
      return foundation::Ok();
    }
  }
  auto drain_r = unload_current();
  if (!drain_r) return drain_r;
  return load(name);
}

foundation::Result<void> BackendCoordinator::swap_to_cancellable(
    const std::string& name, std::chrono::milliseconds timeout) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_loaded_.has_value() && *current_loaded_ == name) {
      return foundation::Ok();
    }
  }
  if (swap_cancel_.load()) {
    reset_swap_cancel();
    return foundation::Err(foundation::ErrorCode::Cancelled, "swap cancelled before start");
  }
  swap_in_progress_.store(true);
  foundation::Result<void> result;
  try {
    auto drain_r = unload_current();
    if (!drain_r) {
      result = drain_r;
    } else if (swap_cancel_.load()) {
      result = foundation::Err(foundation::ErrorCode::Cancelled, "swap cancelled before load");
    } else {
      result = load(name);
    }
  } catch (...) {
    result = foundation::Err(foundation::ErrorCode::Internal, "swap threw");
  }
  swap_in_progress_.store(false);
  reset_swap_cancel();
  (void)timeout;
  return result;
}

bool BackendCoordinator::is_loaded(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(name);
    if (it == instances_.end() || !it->second) return false;
    if (!current_loaded_.has_value() || *current_loaded_ != name) return false;
    return it->second->is_loaded();
}

std::optional<std::string> BackendCoordinator::get_loaded_model() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_loaded_;
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

const IModel* BackendCoordinator::get_model(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(name);
    if (it == instances_.end() || !it->second) return nullptr;
    return it->second.get();
}

foundation::Result<int> BackendCoordinator::acquire_slot(
    const std::string& name, const AcquireSlotOptions& opts) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = clock::now() + opts.timeout;
    while (true) {
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<int>(foundation::ErrorCode::NotFound,
                                         "model not loaded: " + name);
        }
        auto& model = it->second;
        if (!model->is_loaded()) {
            if (!opts.block) {
                return foundation::Err<int>(foundation::ErrorCode::Unavailable,
                                             "model not loaded (non-blocking): " + name);
            }
            if (clock::now() >= deadline) {
                return foundation::Err<int>(foundation::ErrorCode::Timeout,
                                             "timeout waiting for model to load: " + name);
            }
            cv_.wait_until(lock, deadline);
            continue;
        }
        auto slot_r = model->acquire_slot();
        if (slot_r) {
            ++active_requests_;
            return slot_r;
        }
        if (!opts.block) {
            return foundation::Err<int>(foundation::ErrorCode::Unavailable,
                                         "no free slots (non-blocking) for: " + name);
        }
        if (clock::now() >= deadline) {
            return foundation::Err<int>(foundation::ErrorCode::Timeout,
                                         "timeout waiting for slot on: " + name);
        }
        cv_.wait_until(lock, deadline);
    }
}

foundation::Result<void> BackendCoordinator::release_slot(
    const std::string& name, int slot_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(name);
        if (it == instances_.end() || !it->second) {
            return foundation::Err<void>(foundation::ErrorCode::NotFound,
                                          "model not loaded: " + name);
        }
        auto r = it->second->release_slot(slot_id);
        if (!r) return r;
        if (active_requests_ > 0) --active_requests_;
    }
    cv_.notify_all();
    return foundation::Ok();
}

foundation::Result<InferenceResult> BackendCoordinator::predict(
    const std::string& name, int slot_id, const InferenceRequest& req) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = instances_.find(name);
    if (it == instances_.end() || !it->second) {
        return foundation::Err<InferenceResult>(foundation::ErrorCode::NotFound,
                                                  "model not loaded: " + name);
    }
    return it->second->predict(slot_id, req);
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

} // namespace inferdeck::model
