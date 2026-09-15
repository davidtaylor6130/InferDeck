#pragma once

#include <memory>
#include <functional>
#include <string>

#include "model/fixed_backend.hpp"
#include "model/imodel.hpp"

struct sd_ctx_t;

namespace inferdeck::native_runtimes {

class LTXVideoBackend final : public model::FixedBackend, public model::IVideoBackend {
public:
    explicit LTXVideoBackend(model::ModelInfo info);
    ~LTXVideoBackend() override;
    foundation::Result<void> load() override;
    foundation::Result<void> unload() override;
    foundation::Result<model::VideoGenerationResult> generate_video(
        int slot_id, const model::VideoGenerationRequest& request,
        const std::function<bool(int)>& progress = {}) override;

private:
    void release_context() noexcept;
    sd_ctx_t* context_{nullptr};
    std::string diffusion_model_path_;
    std::string high_noise_diffusion_model_path_;
    std::string llm_path_;
    std::string embeddings_connectors_path_;
    std::string vae_path_;
    std::string audio_vae_path_;
    std::string backend_;
    std::string max_vram_;
};

std::unique_ptr<model::IBackend> make_ltx_video_backend(const model::ModelInfo& info);

}
