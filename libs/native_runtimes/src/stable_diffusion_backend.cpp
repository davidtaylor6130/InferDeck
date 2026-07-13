#include "model/fixed_backend.hpp"
#include "model/imodel.hpp"
#include "native_runtimes/png.hpp"

#include <stable-diffusion.h>

#include <chrono>
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace inferdeck::native_runtimes {

namespace {

std::string artifact(const model::ModelInfo& info, const std::string& key,
                     const std::string& fallback = {}) {
    const auto found = info.artifacts.find(key);
    return found == info.artifacts.end() ? fallback : found->second;
}

model::ModelInfo image_info(model::ModelInfo info) {
    info.runtime = "stable_diffusion_cpp";
    info.modality = "image";
    info.capabilities = {"image_generation"};
    info.n_slots = 1;
    info.min_slots = 1;
    return info;
}

std::mutex generation_mutex;

struct ProgressState {
    sd_ctx_t* context{};
    const std::function<bool(int)>* callback{};
    bool cancelled{false};
};

void progress_callback(int step, int steps, float, void* data) {
    auto* state = static_cast<ProgressState*>(data);
    const int percent = steps > 0 ? std::clamp(step * 100 / steps, 0, 100) : 0;
    if (state->callback && *state->callback && !(*state->callback)(percent)) {
        state->cancelled = true;
        sd_cancel_generation(state->context, SD_CANCEL_ALL);
    }
}

class StableDiffusionBackend final : public model::FixedBackend, public model::IImageBackend {
public:
    explicit StableDiffusionBackend(model::ModelInfo info)
        : FixedBackend(image_info(std::move(info))),
          model_path_(artifact(info_, "model", info_.gguf_path)),
          vae_path_(artifact(info_, "vae")), clip_l_path_(artifact(info_, "clip_l")),
          clip_g_path_(artifact(info_, "clip_g")), t5xxl_path_(artifact(info_, "t5xxl")),
          backend_(artifact(info_, "backend", "vulkan")), max_vram_(artifact(info_, "max_vram")) {}

    ~StableDiffusionBackend() override { if (context_) free_sd_ctx(context_); }

    foundation::Result<void> load() override {
        if (model_path_.empty()) return foundation::Err<void>(foundation::ErrorCode::InvalidArgument, "stable-diffusion.cpp model artifact is missing");
        sd_ctx_params_t params;
        sd_ctx_params_init(&params);
        params.model_path = model_path_.c_str();
        params.vae_path = vae_path_.empty() ? nullptr : vae_path_.c_str();
        params.clip_l_path = clip_l_path_.empty() ? nullptr : clip_l_path_.c_str();
        params.clip_g_path = clip_g_path_.empty() ? nullptr : clip_g_path_.c_str();
        params.t5xxl_path = t5xxl_path_.empty() ? nullptr : t5xxl_path_.c_str();
        params.backend = backend_.c_str();
        params.max_vram = max_vram_.empty() ? nullptr : max_vram_.c_str();
        params.n_threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
        params.enable_mmap = true;
        params.flash_attn = true;
        context_ = new_sd_ctx(&params);
        if (!context_ || !sd_ctx_supports_image_generation(context_)) {
            if (context_) free_sd_ctx(context_);
            context_ = nullptr;
            return foundation::Err<void>(foundation::ErrorCode::Unavailable, "stable-diffusion.cpp failed to load image model");
        }
        set_loaded(true);
        return foundation::Ok();
    }

    foundation::Result<void> unload() override {
        set_loaded(false);
        if (context_) free_sd_ctx(context_);
        context_ = nullptr;
        return foundation::Ok();
    }

    foundation::Result<model::ImageGenerationResult> generate_images(
        int, const model::ImageGenerationRequest& request,
        const std::function<bool(int)>& progress) override {
        if (!context_) return foundation::Err<model::ImageGenerationResult>(foundation::ErrorCode::NotLoaded, "image model is not loaded");
        std::lock_guard generation_lock(generation_mutex);
        ProgressState state{context_, &progress, false};
        sd_set_progress_callback(progress_callback, &state);
        sd_img_gen_params_t params;
        sd_img_gen_params_init(&params);
        params.prompt = request.prompt.c_str();
        params.negative_prompt = request.negative_prompt.c_str();
        params.width = request.width;
        params.height = request.height;
        params.seed = request.seed;
        params.batch_count = request.count;
        params.sample_params.sample_steps = request.steps;
        params.sample_params.guidance.txt_cfg = request.guidance_scale;
        sd_image_t* images = nullptr;
        int count = 0;
        const auto started = std::chrono::steady_clock::now();
        const bool generated = generate_image(context_, &params, &images, &count);
        sd_set_progress_callback(nullptr, nullptr);
        if (!generated || !images || count < 1) {
            if (images) free_sd_images(images, count);
            return foundation::Err<model::ImageGenerationResult>(
                state.cancelled ? foundation::ErrorCode::Cancelled : foundation::ErrorCode::Internal,
                state.cancelled ? "image generation cancelled" : "stable-diffusion.cpp image generation failed");
        }
        model::ImageGenerationResult result;
        for (int index = 0; index < count; ++index) {
            auto png = encode_png(images[index].data, static_cast<int>(images[index].width),
                                  static_cast<int>(images[index].height), static_cast<int>(images[index].channel));
            if (!png) {
                free_sd_images(images, count);
                return foundation::Err<model::ImageGenerationResult>(png.error().code, png.error().message);
            }
            result.png_images.push_back(std::move(*png));
        }
        free_sd_images(images, count);
        result.duration_ms = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return foundation::Ok(std::move(result));
    }

private:
    sd_ctx_t* context_{nullptr};
    std::string model_path_;
    std::string vae_path_;
    std::string clip_l_path_;
    std::string clip_g_path_;
    std::string t5xxl_path_;
    std::string backend_;
    std::string max_vram_;
};

}

std::unique_ptr<model::IBackend> make_stable_diffusion_backend(const model::ModelInfo& info) {
    return std::make_unique<StableDiffusionBackend>(info);
}

}
