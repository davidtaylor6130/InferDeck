#pragma once

#include <chrono>
#include <functional>

#include "foundation/result.hpp"
#include "model/model_info.hpp"

namespace inferdeck::model {

struct LifecycleControl {
    using clock = std::chrono::steady_clock;

    clock::time_point deadline{clock::time_point::max()};
    std::function<bool()> cancelled{};

    [[nodiscard]] bool is_cancelled() const {
        return cancelled && cancelled();
    }

    [[nodiscard]] bool is_expired() const {
        return clock::now() >= deadline;
    }
};

class IBackend {
public:
    virtual ~IBackend() = default;

    virtual const ModelInfo& info() const = 0;
    virtual foundation::Result<void> load() = 0;
    virtual foundation::Result<void> unload() = 0;
    virtual foundation::Result<void> load(const LifecycleControl& control) {
        if (control.is_cancelled()) {
            return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                         "backend load cancelled");
        }
        if (control.is_expired()) {
            return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                         "backend load deadline expired");
        }
        auto result = load();
        if (!result) return result;
        if (control.is_cancelled()) {
            (void)unload();
            return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                         "backend load cancelled");
        }
        if (control.is_expired()) {
            (void)unload();
            return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                         "backend load deadline expired");
        }
        return result;
    }
    virtual foundation::Result<void> unload(const LifecycleControl& control) {
        if (control.is_cancelled()) {
            return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                         "backend unload cancelled");
        }
        if (control.is_expired()) {
            return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                         "backend unload deadline expired");
        }
        return unload();
    }
    virtual bool is_loaded() const = 0;
    virtual int vram_usage_mb() const = 0;
    virtual int n_slots() const = 0;
    virtual int n_free_slots() const = 0;
    virtual int min_slots() const { return n_slots(); }
    virtual bool can_resize_slots() const { return false; }
    virtual int estimate_vram_mb(int slots) const {
        (void)slots;
        return vram_usage_mb();
    }
    virtual foundation::Result<void> resize_slots(int slots) {
        (void)slots;
        return foundation::Err<void>(foundation::ErrorCode::Unavailable,
                                     "backend capacity is fixed");
    }
    virtual foundation::Result<void> resize_slots(
        int slots, const LifecycleControl& control) {
        if (control.is_cancelled()) {
            return foundation::Err<void>(foundation::ErrorCode::Cancelled,
                                         "backend resize cancelled");
        }
        if (control.is_expired()) {
            return foundation::Err<void>(foundation::ErrorCode::Timeout,
                                         "backend resize deadline expired");
        }
        return resize_slots(slots);
    }
    virtual foundation::Result<int> acquire_slot() = 0;
    virtual foundation::Result<void> release_slot(int slot_id) = 0;
    virtual bool slot_busy(int slot_id) const = 0;
    virtual foundation::Result<void> reset_all_slots() {
        return foundation::Ok();
    }
    virtual void request_cancel() {}
};

} // namespace inferdeck::model
