#include "native_runtimes/ltx_video_backend.hpp"

#include "native_runtimes/diffusion_runtime_lock.hpp"
#include "native_runtimes/ltx_video_encoder.hpp"

#include <stable-diffusion.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <thread>
#include <utility>

namespace inferdeck::native_runtimes {
namespace {
std::string artifact(const model::ModelInfo& info, const std::string& key,
                     const std::string& fallback = {}) {
    const auto found = info.artifacts.find(key);
    return found == info.artifacts.end() ? fallback : found->second;
}

model::ModelInfo video_info(model::ModelInfo info) {
    info.runtime = "ltx_video_cpp";
    info.modality = "video";
    info.capabilities = {"video_generation"};
    info.n_slots = 1;
    info.min_slots = 1;
    return info;
}

struct ProgressState {
    sd_ctx_t* context{};
    const std::function<bool(int)>* callback{};
    std::atomic<bool> cancelled{false};
};

void progress_callback(int step, int steps, float, void* data) {
    auto* state = static_cast<ProgressState*>(data);
    if (!state || !state->callback || !*state->callback) return;
    const int percent = steps > 0 ? std::clamp(step * 100 / steps, 0, 100) : 0;
    try {
        if (!(*state->callback)(percent)) {
            state->cancelled.store(true);
            sd_cancel_generation(state->context, SD_CANCEL_ALL);
        }
    } catch (...) {
        state->cancelled.store(true);
        sd_cancel_generation(state->context, SD_CANCEL_ALL);
    }
}

struct GeneratedVideo {
    sd_image_t* frames{};
    int count{};
    sd_audio_t* audio{};
    ~GeneratedVideo() {
        if (frames) free_sd_images(frames, count);
        if (audio) free_sd_audio(audio);
    }
};

struct ProgressCallbackReset {
    ~ProgressCallbackReset() { sd_set_progress_callback(nullptr, nullptr); }
};

foundation::Result<void> validate(const model::VideoGenerationRequest& request) {
    if (request.prompt.empty()) return foundation::Err<void>(
        foundation::ErrorCode::InvalidArgument, "video prompt must not be empty");
    if (request.prompt.size() > 32000 || request.negative_prompt.size() > 32768) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     "video prompt exceeds the request limit");
    }
    if (request.seed < -1 ||
        request.seed > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     "video seed is outside the supported range");
    }
    if (request.width < 64 || request.height < 64 || request.width > 1280 ||
        request.height > 720 || request.width % 32 != 0 ||
        request.height % 32 != 0 ||
        static_cast<std::int64_t>(request.width) * request.height > 1280LL * 720LL) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     "video dimensions are outside the bounded profile");
    }
    if (request.frames < 9 || request.frames > 121 || (request.frames - 1) % 8 != 0) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     "video frames must be 8n+1 between 9 and 121");
    }
    if (request.fps <= 0 || request.fps > 60 || request.steps <= 0 ||
        request.steps > 50 || !std::isfinite(request.guidance_scale) ||
        request.guidance_scale < 0.0f || request.guidance_scale > 20.0f) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     "video generation settings are outside the bounded profile");
    }
    return foundation::Ok();
}
}

LTXVideoBackend::LTXVideoBackend(model::ModelInfo info)
    : FixedBackend(video_info(std::move(info))),
      diffusion_model_path_(artifact(info_, "diffusion_model",
                                     artifact(info_, "model", info_.gguf_path))),
      high_noise_diffusion_model_path_(artifact(info_, "high_noise_diffusion_model")),
      llm_path_(artifact(info_, "llm", artifact(info_, "text_encoder"))),
      embeddings_connectors_path_(artifact(info_, "embeddings_connectors")),
      vae_path_(artifact(info_, "vae")),
      audio_vae_path_(artifact(info_, "audio_vae")),
      backend_(artifact(info_, "backend", "diffusion=vulkan0,te=cpu")),
      max_vram_(artifact(info_, "max_vram")) {}

LTXVideoBackend::~LTXVideoBackend() {
    std::lock_guard lock(diffusion_runtime_mutex());
    release_context();
}

foundation::Result<void> LTXVideoBackend::load() {
    std::lock_guard lock(diffusion_runtime_mutex());
    set_loaded(false);
    release_context();
    if (diffusion_model_path_.empty() || llm_path_.empty() ||
        embeddings_connectors_path_.empty() || vae_path_.empty()) {
        return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                     "LTX video model artifacts are incomplete");
    }
    sd_ctx_params_t params;
    sd_ctx_params_init(&params);
    params.diffusion_model_path = diffusion_model_path_.c_str();
    params.high_noise_diffusion_model_path = high_noise_diffusion_model_path_.empty()
        ? nullptr : high_noise_diffusion_model_path_.c_str();
    params.llm_path = llm_path_.c_str();
    params.embeddings_connectors_path = embeddings_connectors_path_.c_str();
    params.vae_path = vae_path_.c_str();
    params.audio_vae_path = audio_vae_path_.empty() ? nullptr : audio_vae_path_.c_str();
    params.backend = backend_.c_str();
    params.max_vram = max_vram_.empty() ? nullptr : max_vram_.c_str();
    params.n_threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
    params.enable_mmap = true;
    params.flash_attn = true;
    params.diffusion_flash_attn = true;
    params.auto_fit = backend_ == "vulkan";
    try {
        context_ = new_sd_ctx(&params);
    } catch (const std::exception& error) {
        release_context();
        return foundation::Err<void>(
            foundation::ErrorCode::Internal,
            std::string("stable-diffusion.cpp failed to load LTX video model: ") + error.what());
    } catch (...) {
        release_context();
        return foundation::Err<void>(
            foundation::ErrorCode::Internal,
            "stable-diffusion.cpp failed to load LTX video model");
    }
    if (!context_ || !sd_ctx_supports_video_generation(context_)) {
        release_context();
        return foundation::Err<void>(foundation::ErrorCode::Unavailable,
                                     "stable-diffusion.cpp failed to load LTX video model");
    }
    set_loaded(true);
    return foundation::Ok();
}

foundation::Result<void> LTXVideoBackend::unload() {
    std::lock_guard lock(diffusion_runtime_mutex());
    set_loaded(false);
    release_context();
    return foundation::Ok();
}

foundation::Result<model::VideoGenerationResult> LTXVideoBackend::generate_video(
    int, const model::VideoGenerationRequest& request,
    const std::function<bool(int)>& progress) {
    const auto valid = validate(request);
    if (!valid) return foundation::Err<model::VideoGenerationResult>(
        valid.error().code, valid.error().message, valid.error().field);
    std::lock_guard lock(diffusion_runtime_mutex());
    if (!context_) return foundation::Err<model::VideoGenerationResult>(
        foundation::ErrorCode::NotLoaded, "LTX video model is not loaded");
    ProgressState state{context_, &progress, false};
    sd_set_progress_callback(progress_callback, &state);
    ProgressCallbackReset callback_reset;
    sd_vid_gen_params_t params;
    sd_vid_gen_params_init(&params);
    params.prompt = request.prompt.c_str();
    params.negative_prompt = request.negative_prompt.c_str();
    params.width = request.width;
    params.height = request.height;
    params.video_frames = request.frames;
    params.fps = request.fps;
    params.seed = request.seed;
    params.sample_params.sample_steps = request.steps;
    params.high_noise_sample_params = params.sample_params;
    params.sample_params.guidance.txt_cfg = request.guidance_scale;
    params.high_noise_sample_params.guidance.txt_cfg = request.guidance_scale;
    GeneratedVideo generated;
    const auto started = std::chrono::steady_clock::now();
    bool success = false;
    try {
        success = ::generate_video(context_, &params, &generated.frames,
                                   &generated.count, &generated.audio);
    } catch (const std::exception& error) {
        return foundation::Err<model::VideoGenerationResult>(
            foundation::ErrorCode::Internal,
            std::string("stable-diffusion.cpp video generation failed: ") + error.what());
    } catch (...) {
        return foundation::Err<model::VideoGenerationResult>(
            foundation::ErrorCode::Internal, "stable-diffusion.cpp video generation failed");
    }
    if (state.cancelled.load() || !success || !generated.frames || generated.count <= 0) {
        return foundation::Err<model::VideoGenerationResult>(
            state.cancelled.load() ? foundation::ErrorCode::Cancelled : foundation::ErrorCode::Internal,
            state.cancelled.load() ? "LTX video generation cancelled" :
                              "stable-diffusion.cpp video generation failed");
    }
    if (generated.count > 121) {
        return foundation::Err<model::VideoGenerationResult>(
            foundation::ErrorCode::Internal, "stable-diffusion.cpp returned too many video frames");
    }
    for (int index = 0; index < generated.count; ++index) {
        if (generated.frames[index].data == nullptr ||
            generated.frames[index].width != static_cast<std::uint32_t>(request.width) ||
            generated.frames[index].height != static_cast<std::uint32_t>(request.height)) {
            return foundation::Err<model::VideoGenerationResult>(
                foundation::ErrorCode::Internal,
                "stable-diffusion.cpp returned inconsistent video frames");
        }
    }
    foundation::Result<std::vector<std::byte>> encoded;
    try {
        encoded = encode_ltx_video(generated.frames, generated.count,
                                   request.fps, generated.audio);
    } catch (const std::exception& error) {
        return foundation::Err<model::VideoGenerationResult>(
            foundation::ErrorCode::Internal,
            std::string("stable-diffusion.cpp video encoding failed: ") + error.what());
    } catch (...) {
        return foundation::Err<model::VideoGenerationResult>(
            foundation::ErrorCode::Internal, "stable-diffusion.cpp video encoding failed");
    }
    if (!encoded) return foundation::Err<model::VideoGenerationResult>(
        encoded.error().code, encoded.error().message, encoded.error().field);
    model::VideoGenerationResult result;
    result.video_bytes = std::move(*encoded);
    result.content_type = ltx_video_content_type();
    result.duration_ms = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    result.output_video_seconds = static_cast<double>(generated.count) / request.fps;
    return foundation::Ok(std::move(result));
}

void LTXVideoBackend::release_context() noexcept {
    if (context_) free_sd_ctx(context_);
    context_ = nullptr;
}

std::unique_ptr<model::IBackend> make_ltx_video_backend(const model::ModelInfo& info) {
    return std::make_unique<LTXVideoBackend>(info);
}
}
