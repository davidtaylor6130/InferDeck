#pragma once

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include "model/ibackend.hpp"

namespace inferdeck::model {

class FixedBackend : public IBackend {
public:
    explicit FixedBackend(ModelInfo info) : info_(std::move(info)), slots_(info_.n_slots, false) {}

    const ModelInfo& info() const override { return info_; }
    bool is_loaded() const override { std::lock_guard lock(mutex_); return loaded_; }
    int vram_usage_mb() const override { return is_loaded() ? info_.vram_required_mb : 0; }
    int n_slots() const override { return static_cast<int>(slots_.size()); }
    int min_slots() const override { return info_.min_slots; }
    int n_free_slots() const override {
        std::lock_guard lock(mutex_);
        return static_cast<int>(std::count(slots_.begin(), slots_.end(), false));
    }
    foundation::Result<int> acquire_slot() override {
        std::lock_guard lock(mutex_);
        if (!loaded_) return foundation::Err<int>(foundation::ErrorCode::NotLoaded, "backend is not loaded");
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (!slots_[index]) { slots_[index] = true; return foundation::Ok(static_cast<int>(index)); }
        }
        return foundation::Err<int>(foundation::ErrorCode::Unavailable, "no free slots");
    }
    foundation::Result<void> release_slot(int slot_id) override {
        std::lock_guard lock(mutex_);
        if (slot_id < 0 || static_cast<std::size_t>(slot_id) >= slots_.size()) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument, "invalid slot");
        }
        slots_[slot_id] = false;
        return foundation::Ok();
    }
    bool slot_busy(int slot_id) const override {
        std::lock_guard lock(mutex_);
        return slot_id >= 0 && static_cast<std::size_t>(slot_id) < slots_.size() && slots_[slot_id];
    }

protected:
    void set_loaded(bool loaded) {
        std::lock_guard lock(mutex_);
        loaded_ = loaded;
        if (!loaded) std::fill(slots_.begin(), slots_.end(), false);
    }
    ModelInfo info_;

private:
    mutable std::mutex mutex_;
    bool loaded_{false};
    std::vector<bool> slots_;
};

}
