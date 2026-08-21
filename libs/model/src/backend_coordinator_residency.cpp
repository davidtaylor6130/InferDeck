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

}
