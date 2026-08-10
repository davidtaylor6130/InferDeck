#include "model/backend_coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
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
    std::lock_guard<std::recursive_mutex> swap_lock(swap_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = instances_.find(name);
        if (existing != instances_.end() && existing->second &&
            existing->second->is_loaded()) {
            if (existing->second->estimate_vram_mb(existing->second->n_slots()) > 0) {
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
    }
    auto& inst = instances_[name];
    auto r = inst->load();
    if (!r) return r;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inst->estimate_vram_mb(inst->n_slots()) > 0) {
            current_loaded_ = name;
            ++resource_generation_;
        }
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
    std::lock_guard<std::recursive_mutex> swap_lock(swap_mutex_);
    auto drain_deadline = clock::now() + std::chrono::milliseconds{30000};
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
        while (has_active_requests() && clock::now() < drain_deadline) {
            cv_.wait_for(lock, std::chrono::milliseconds{100});
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

    auto r = instance ? instance->unload() : foundation::Ok();

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
  std::lock_guard<std::recursive_mutex> swap_lock(swap_mutex_);
  if (is_loaded(name)) return foundation::Ok();
  if (vram_budget_mb() <= 0) {
    auto drain_r = unload_current();
    if (!drain_r) return drain_r;
    return load(name);
  }
  const auto before = residency();
  const auto previous_primary = get_loaded_model();
  auto capacity = prepare_capacity_for(name);
  if (!capacity) return capacity;
  auto loaded = load(name);
  if (loaded) return loaded;

  (void)unload(name);
  for (const auto& prior : before) {
    const auto* backend = get_backend(prior.name);
    if (backend && backend->is_loaded() && backend->n_slots() == prior.slots) continue;
    if (backend && backend->is_loaded()) (void)unload(prior.name);
    (void)load(prior.name);
  }
  if (previous_primary && is_loaded(*previous_primary)) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_loaded_ = *previous_primary;
    last_resource_decision_ = "load failed for " + name + "; previous residency restored";
  }
  return loaded;
}

foundation::Result<void> BackendCoordinator::swap_to_cancellable(
    const std::string& name, std::chrono::milliseconds timeout) {
  std::lock_guard<std::recursive_mutex> swap_lock(swap_mutex_);
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
      result = swap_to(name);
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
    return it->second->is_loaded();
}

std::optional<std::string> BackendCoordinator::get_loaded_model() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_loaded_;
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
        out.push_back({name, backend->info().runtime, backend->info().modality,
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
        if (!waiters_.empty() || resizing_models_.contains(name)) {
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
                        clock::now(), deadline, opts.cancelled, false, std::nullopt});
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
    std::sort(ordered.begin(), ordered.end(), [now](const auto* a, const auto* b) {
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

bool BackendCoordinator::waiter_is_next_locked(std::uint64_t id, time_point now) const {
    const SlotWaiter* selected = nullptr;
    std::int64_t selected_score = 0;
    for (const auto& waiter : waiters_) {
        if (waiter.retry_after_generation &&
            *waiter.retry_after_generation == resource_generation_) {
            continue;
        }
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
    int selected_vram = 0;
    for (const auto& [name, backend] : instances_) {
        if (backend && backend->is_loaded()) {
            const int vram = backend->estimate_vram_mb(backend->n_slots());
            if (vram <= 0 || vram < selected_vram ||
                (vram == selected_vram && current_loaded_ && name > *current_loaded_)) {
                continue;
            }
            current_loaded_ = name;
            selected_vram = vram;
        }
    }
}

foundation::Result<void> BackendCoordinator::prepare_capacity_for(const std::string& name) {
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
        auto resized = backend->resize_slots(next_slots);
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
                    (active == active_requests_by_model_.end() || active->second == 0)) {
                    candidate = loaded_name;
                    candidate_vram = estimated;
                }
            }
        }
        if (candidate.empty()) break;
        auto unloaded = unload(candidate);
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

} // namespace inferdeck::model
