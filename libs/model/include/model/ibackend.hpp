#pragma once

#include "foundation/result.hpp"
#include "model/model_info.hpp"

namespace inferdeck::model {

class IBackend {
public:
    virtual ~IBackend() = default;

    virtual const ModelInfo& info() const = 0;
    virtual foundation::Result<void> load() = 0;
    virtual foundation::Result<void> unload() = 0;
    virtual bool is_loaded() const = 0;
    virtual int vram_usage_mb() const = 0;
    virtual int n_slots() const = 0;
    virtual int n_free_slots() const = 0;
    virtual foundation::Result<int> acquire_slot() = 0;
    virtual foundation::Result<void> release_slot(int slot_id) = 0;
    virtual bool slot_busy(int slot_id) const = 0;
    virtual foundation::Result<void> reset_all_slots() {
        return foundation::Ok();
    }
    virtual void request_cancel() {}
};

} // namespace inferdeck::model
