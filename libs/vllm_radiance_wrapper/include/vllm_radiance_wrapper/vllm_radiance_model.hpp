#pragma once

#include <atomic>
#include <memory>

#include "model/imodel.hpp"

namespace inferdeck::vllm_radiance_wrapper {

class VllmRadianceModel final : public model::IModel {
public:
    explicit VllmRadianceModel(model::ModelInfo info);
    ~VllmRadianceModel() override;

    const model::ModelInfo& info() const override;
    foundation::Result<void> load() override;
    foundation::Result<void> unload() override;
    foundation::Result<void> load(const model::LifecycleControl& control) override;
    foundation::Result<void> unload(const model::LifecycleControl& control) override;
    bool is_loaded() const override;
    bool execution_healthy() const override;
    int vram_usage_mb() const override;
    int n_slots() const override;
    int n_free_slots() const override;
    foundation::Result<int> acquire_slot() override;
    foundation::Result<void> release_slot(int slot_id) override;
    bool slot_busy(int slot_id) const override;
    void request_cancel() override;

    foundation::Result<model::InferenceResult> predict(
        int slot_id, const model::InferenceRequest& request) override;
    foundation::Result<model::InferenceResult> predict_cancellable(
        int slot_id, const model::InferenceRequest& request,
        const std::atomic<bool>* cancel) override;
    foundation::Result<model::InferenceResult> predict_stream(
        int slot_id, const model::InferenceRequest& request,
        const TokenCallback& callback, const std::atomic<bool>* cancel) override;

private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace inferdeck::vllm_radiance_wrapper
