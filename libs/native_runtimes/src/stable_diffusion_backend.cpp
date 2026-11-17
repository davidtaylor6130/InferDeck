#include "foundation/logging.hpp"
#include "model/fixed_backend.hpp"
#include "model/imodel.hpp"
#include "native_runtimes/image_memory_policy.hpp"
#include "native_runtimes/png.hpp"

#include <stable-diffusion.h>
#include <native_runtimes/diffusion_runtime_lock.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

std::mutex& generation_mutex = diffusion_runtime_mutex();

struct ProgressState {
    sd_ctx_t* context{};
    const std::function<bool(int)>* callback{};
    std::atomic<bool> cancelled{false};
};

void progress_callback(int step, int steps, float, void* data) {
    auto* state = static_cast<ProgressState*>(data);
    try {
        if (!state) return;
        const int percent = steps > 0 ? std::clamp(step * 100 / steps, 0, 100) : 0;
        if (state->callback && *state->callback && !(*state->callback)(percent)) {
            state->cancelled.store(true);
            sd_cancel_generation(state->context, SD_CANCEL_ALL);
        }
    } catch (...) {
        if (state) {
            state->cancelled.store(true);
            sd_cancel_generation(state->context, SD_CANCEL_ALL);
        }
    }
}

struct GenerationResources {
    explicit GenerationResources(ProgressState& state) {
        sd_set_progress_callback(progress_callback, &state);
    }
    ~GenerationResources() {
        sd_set_progress_callback(nullptr, nullptr);
        free_sd_image_generation_result(generated);
    }
    GenerationResources(const GenerationResources&) = delete;
    GenerationResources& operator=(const GenerationResources&) = delete;
    sd_image_generation_result_t* generated{};
};

class StableDiffusionBackend final : public model::FixedBackend, public model::IImageBackend {
public:
    explicit StableDiffusionBackend(model::ModelInfo info)
        : FixedBackend(image_info(std::move(info))),
          model_path_(artifact(info_, "model", info_.gguf_path)),
          diffusion_model_path_(artifact(info_, "diffusion_model")),
          llm_path_(artifact(info_, "llm")), llm_vision_path_(artifact(info_, "llm_vision")),
          model_args_(artifact(info_, "model_args")),
          default_steps_(std::max(1, std::atoi(artifact(info_, "steps", "20").c_str()))),
          default_guidance_scale_(std::max(0.0f, static_cast<float>(std::atof(artifact(info_, "guidance_scale", "7.0").c_str())))),
          vae_path_(artifact(info_, "vae")), clip_l_path_(artifact(info_, "clip_l")),
          clip_g_path_(artifact(info_, "clip_g")), t5xxl_path_(artifact(info_, "t5xxl")),
          backend_(artifact(info_, "backend", "vulkan")), max_vram_(artifact(info_, "max_vram")) {}

    ~StableDiffusionBackend() override {
        std::lock_guard generation_lock(generation_mutex);
        release_context();
    }

    foundation::Result<void> load() override {
        std::lock_guard generation_lock(generation_mutex);
        set_loaded(false);
        release_context();
        if (model_path_.empty() && diffusion_model_path_.empty()) return foundation::Err<void>(foundation::ErrorCode::InvalidArgument, "stable-diffusion.cpp model or diffusion_model artifact is missing");
        const auto optional_path = [](const std::string& path) { return path.empty() ? nullptr : path.c_str(); };
        install_diffusion_logging();
        sd_ctx_params_t params;
        sd_ctx_params_init(&params);
        params.model_path = optional_path(model_path_);
        params.diffusion_model_path = optional_path(diffusion_model_path_);
        params.llm_path = optional_path(llm_path_);
        params.llm_vision_path = optional_path(llm_vision_path_);
        params.model_args = optional_path(model_args_);
        params.vae_path = vae_path_.empty() ? nullptr : vae_path_.c_str();
        params.clip_l_path = clip_l_path_.empty() ? nullptr : clip_l_path_.c_str();
        params.clip_g_path = clip_g_path_.empty() ? nullptr : clip_g_path_.c_str();
        params.t5xxl_path = t5xxl_path_.empty() ? nullptr : t5xxl_path_.c_str();
        params.backend = backend_.c_str();
        params.max_vram = max_vram_.empty() ? nullptr : max_vram_.c_str();
        params.n_threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
        params.enable_mmap = true;
        params.flash_attn = true;
        params.auto_fit = backend_ == "vulkan";
        foundation::LOG_INFO("image_backend_selection", "model={} backend={} auto_fit={}",
                             info_.name, backend_, params.auto_fit);
        const bool use_direct_convolution = image_use_direct_convolution(backend_);
        params.diffusion_conv_direct = use_direct_convolution;
        params.vae_conv_direct = use_direct_convolution;
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
        std::lock_guard generation_lock(generation_mutex);
        set_loaded(false);
        release_context();
        return foundation::Ok();
    }

    foundation::Result<model::ImageGenerationResult> generate_images(
        int, const model::ImageGenerationRequest& request,
        const std::function<bool(int)>& progress) override {
        std::lock_guard generation_lock(generation_mutex);
        if (!context_) return foundation::Err<model::ImageGenerationResult>(foundation::ErrorCode::NotLoaded, "image model is not loaded");
        try {
            ProgressState state{context_, &progress, false};
            GenerationResources resources(state);
            sd_img_gen_params_t params;
            sd_img_gen_params_init(&params);
            params.prompt = request.prompt.c_str();
            params.negative_prompt = request.negative_prompt.c_str();
            params.width = request.width;
            params.height = request.height;
            params.seed = request.seed;
            params.batch_count = request.count;
            params.sample_params.sample_steps = request.steps > 0 ? request.steps : default_steps_;
            params.sample_params.guidance.txt_cfg =
                request.guidance_scale >= 0.0f ? request.guidance_scale : default_guidance_scale_;
            const int vae_tile_size = image_vae_tile_size(request.width, request.height);
            params.vae_tiling_params.enabled = vae_tile_size > 0;
            params.vae_tiling_params.tile_size_w = vae_tile_size;
            params.vae_tiling_params.tile_size_h = vae_tile_size;
            std::vector<DecodedImage> references;
            for (const std::vector<std::byte>& encoded : request.reference_images) {
                auto decoded = decode_image_rgb(encoded);
                if (!decoded) {
                    return foundation::Err<model::ImageGenerationResult>(decoded.error().code, decoded.error().message);
                }
                references.push_back(std::move(*decoded));
            }
            std::vector<sd_image_t> reference_views;
            for (DecodedImage& reference : references) {
                reference_views.push_back({static_cast<uint32_t>(reference.width),
                                           static_cast<uint32_t>(reference.height), 3, reference.rgb.data()});
            }
            params.ref_images = reference_views.empty() ? nullptr : reference_views.data();
            params.ref_images_count = static_cast<int>(reference_views.size());
            const auto started = std::chrono::steady_clock::now();
            const bool generated = generate_image(context_, &params, &resources.generated);
            const int count = resources.generated ? resources.generated->count : 0;
            if (state.cancelled.load() || !generated || count < 1 || !resources.generated->items) {
                return foundation::Err<model::ImageGenerationResult>(
                    state.cancelled.load() ? foundation::ErrorCode::Cancelled : foundation::ErrorCode::Internal,
                    state.cancelled.load() ? "image generation cancelled" : "stable-diffusion.cpp image generation failed");
            }
            model::ImageGenerationResult result;
            for (int index = 0; index < count; ++index) {
                const sd_image_t& image = resources.generated->items[index].image;
                auto png = encode_png(image.data, static_cast<int>(image.width),
                                      static_cast<int>(image.height), static_cast<int>(image.channel));
                if (!png) {
                    return foundation::Err<model::ImageGenerationResult>(png.error().code, png.error().message);
                }
                result.png_images.push_back(std::move(*png));
            }
            result.duration_ms = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            return foundation::Ok(std::move(result));
        } catch (const std::exception& error) {
            return foundation::Err<model::ImageGenerationResult>(
                foundation::ErrorCode::Internal,
                std::string("stable-diffusion.cpp image generation failed: ") + error.what());
        } catch (...) {
            return foundation::Err<model::ImageGenerationResult>(
                foundation::ErrorCode::Internal,
                "stable-diffusion.cpp image generation failed");
        }
    }

private:
    void release_context() noexcept {
        if (context_) free_sd_ctx(context_);
        context_ = nullptr;
    }

    sd_ctx_t* context_{nullptr};
    std::string model_path_;
    std::string diffusion_model_path_;
    std::string llm_path_;
    std::string llm_vision_path_;
    std::string model_args_;
    int default_steps_;
    float default_guidance_scale_;
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
