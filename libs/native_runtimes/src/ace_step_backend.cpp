#include "model/fixed_backend.hpp"
#include "model/imodel.hpp"

#include "audio-io.h"
#include "model-store.h"
#include "pipeline-synth.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace inferdeck::native_runtimes {

namespace {

std::string artifact(const model::ModelInfo& info, const std::string& key,
                     const std::string& fallback = {}) {
    const std::map<std::string, std::string>::const_iterator found =
        info.artifacts.find(key);
    return found == info.artifacts.end() ? fallback : found->second;
}

model::ModelInfo audio_info(model::ModelInfo info) {
    info.runtime = "ace_step_cpp";
    info.modality = "audio_generation";
    info.capabilities = {"audio_generation"};
    info.n_slots = 1;
    info.min_slots = 1;
    return info;
}

foundation::Result<void> require_file(const std::string& path,
                                      const std::string& artifact_name) {
    if (path.empty()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "ACE-Step " + artifact_name + " artifact is missing");
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return foundation::Err<void>(
            foundation::ErrorCode::NotFound,
            "ACE-Step " + artifact_name + " artifact is unavailable");
    }
    return foundation::Ok();
}

std::timed_mutex generation_mutex;

struct ProgressState {
    const std::function<bool(int)>* callback{};
    int progress{0};
    bool cancelled{false};

    bool report() noexcept {
        try {
            if (callback && *callback && !(*callback)(progress)) {
                cancelled = true;
            }
        } catch (...) {
            cancelled = true;
        }
        return !cancelled;
    }
};

bool cancel_generation(void* data) {
    ProgressState* state = static_cast<ProgressState*>(data);
    return !state || !state->report();
}

struct AudioGuard {
    AceAudio* audio{};
    ~AudioGuard() {
        if (audio) {
            ace_audio_free(audio);
        }
    }
};

struct RetainedVramRefresh {
    const ModelStore* store{};
    std::atomic<std::size_t>* bytes{};

    ~RetainedVramRefresh() {
        if (bytes) {
            bytes->store(store_vram_bytes(store), std::memory_order_relaxed);
        }
    }
};

class AceStepBackend final : public model::FixedBackend,
                             public model::IAudioGenerationBackend {
public:
    explicit AceStepBackend(model::ModelInfo info)
        : FixedBackend(audio_info(std::move(info))),
          text_encoder_path_(artifact(info_, "text_encoder")),
          dit_path_(artifact(info_, "dit",
                             artifact(info_, "model", info_.gguf_path))),
          vae_path_(artifact(info_, "vae")),
          adapter_path_(artifact(info_, "adapter")) {}

    ~AceStepBackend() override {
        std::lock_guard<std::timed_mutex> lock(generation_mutex);
        release_context();
    }

    foundation::Result<void> load() override {
        std::lock_guard<std::timed_mutex> lock(generation_mutex);
        set_loaded(false);
        release_context();
        foundation::Result<void> text_encoder =
            require_file(text_encoder_path_, "text_encoder");
        if (!text_encoder) {
            return text_encoder;
        }
        foundation::Result<void> dit = require_file(dit_path_, "dit");
        if (!dit) {
            return dit;
        }
        foundation::Result<void> vae = require_file(vae_path_, "vae");
        if (!vae) {
            return vae;
        }
        if (!adapter_path_.empty()) {
            foundation::Result<void> adapter =
                require_file(adapter_path_, "adapter");
            if (!adapter) {
                return adapter;
            }
        }
        try {
            store_ = store_create(EVICT_NEVER);
            if (!store_) {
                return foundation::Err<void>(
                    foundation::ErrorCode::Internal,
                    "ACE-Step failed to create its model store");
            }
            AceSynthParams params;
            ace_synth_default_params(&params);
            params.text_encoder_path = text_encoder_path_.c_str();
            params.dit_path = dit_path_.c_str();
            params.vae_path = vae_path_.c_str();
            params.adapter_path =
                adapter_path_.empty() ? nullptr : adapter_path_.c_str();
            context_ = ace_synth_load(store_, &params);
            if (!context_) {
                release_context();
                return foundation::Err<void>(
                    foundation::ErrorCode::Unavailable,
                    "ACE-Step failed to load audio model metadata");
            }
        } catch (const std::exception& error) {
            release_context();
            return foundation::Err<void>(
                foundation::ErrorCode::Internal,
                std::string("ACE-Step load failed: ") + error.what());
        } catch (...) {
            release_context();
            return foundation::Err<void>(
                foundation::ErrorCode::Internal,
                "ACE-Step load failed");
        }
        set_loaded(true);
        retained_vram_bytes_.store(
            store_vram_bytes(store_), std::memory_order_relaxed);
        return foundation::Ok();
    }

    foundation::Result<void> unload() override {
        set_loaded(false);
        std::lock_guard<std::timed_mutex> lock(generation_mutex);
        release_context();
        return foundation::Ok();
    }

    int additional_vram_reserve_mb() const override {
        constexpr std::size_t bytes_per_mb = 1024U * 1024U;
        const int required_mb = (std::max)(0, info_.vram_required_mb);
        const std::size_t retained_bytes =
            retained_vram_bytes_.load(std::memory_order_relaxed);
        const std::size_t retained_mb =
            retained_bytes / bytes_per_mb +
            (retained_bytes % bytes_per_mb == 0 ? 0U : 1U);
        if (retained_mb >= static_cast<std::size_t>(required_mb)) {
            return 0;
        }
        return required_mb - static_cast<int>(retained_mb);
    }

    bool live_vram_accounting_complete() const override { return true; }

    foundation::Result<model::AudioGenerationResult> generate_audio(
        int, const model::AudioGenerationRequest& request,
        const std::function<bool(int)>& progress) override {
        ProgressState state{&progress, 0, false};
        std::unique_lock<std::timed_mutex> lock(
            generation_mutex, std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds{100})) {
            if (!state.report()) {
                return foundation::Err<model::AudioGenerationResult>(
                    foundation::ErrorCode::Cancelled,
                    "audio generation cancelled");
            }
        }
        if (!context_ || !store_) {
            return foundation::Err<model::AudioGenerationResult>(
                foundation::ErrorCode::NotLoaded,
                "audio model is not loaded");
        }
        RetainedVramRefresh retained_vram_refresh{
            store_, &retained_vram_bytes_};
        try {
            AceRequest ace_request;
            request_init(&ace_request);
            ace_request.caption = request.prompt;
            ace_request.lyrics = request.lyrics.empty()
                ? "[Instrumental]" : request.lyrics;
            if (request.lyrics.empty()) {
                ace_request.vocal_language = "unknown";
            }
            ace_request.duration = request.duration_seconds;
            ace_request.seed = request.seed;
            ace_request.inference_steps = request.steps;
            ace_request.guidance_scale = request.guidance_scale;
            request_resolve_seed(&ace_request);

            state.progress = 10;
            if (!state.report()) {
                return foundation::Err<model::AudioGenerationResult>(
                    foundation::ErrorCode::Cancelled,
                    "audio generation cancelled");
            }
            const std::chrono::steady_clock::time_point started =
                std::chrono::steady_clock::now();
            AceSynthJob* raw_job = ace_synth_job_run_dit(
                context_, &ace_request, nullptr, 0, nullptr, 0, nullptr, 0,
                nullptr, 0, 1, cancel_generation, &state);
            std::unique_ptr<AceSynthJob, void (*)(AceSynthJob*)> job(
                raw_job, ace_synth_job_free);
            if (!job) {
                return foundation::Err<model::AudioGenerationResult>(
                    state.cancelled ? foundation::ErrorCode::Cancelled
                                    : foundation::ErrorCode::Internal,
                    state.cancelled ? "audio generation cancelled"
                                    : "ACE-Step DiT generation failed");
            }

            state.progress = 80;
            if (!state.report()) {
                return foundation::Err<model::AudioGenerationResult>(
                    foundation::ErrorCode::Cancelled,
                    "audio generation cancelled");
            }
            AceAudio audio{};
            AudioGuard audio_guard{&audio};
            if (ace_synth_job_run_vae(
                    context_, job.get(), &audio,
                    cancel_generation, &state) != 0 ||
                !audio.samples || audio.n_samples <= 0 ||
                audio.sample_rate <= 0) {
                return foundation::Err<model::AudioGenerationResult>(
                    state.cancelled ? foundation::ErrorCode::Cancelled
                                    : foundation::ErrorCode::Internal,
                    state.cancelled ? "audio generation cancelled"
                                    : "ACE-Step VAE decoding failed");
            }

            state.progress = 95;
            if (!state.report()) {
                return foundation::Err<model::AudioGenerationResult>(
                    foundation::ErrorCode::Cancelled,
                    "audio generation cancelled");
            }
            audio_normalize(
                audio.samples, audio.n_samples * 2, ace_request.peak_clip);
            const std::string wave = audio_encode_wav(
                audio.samples, audio.n_samples, audio.sample_rate, WAV_S16);
            if (wave.empty()) {
                return foundation::Err<model::AudioGenerationResult>(
                    foundation::ErrorCode::Internal,
                    "ACE-Step WAV encoding failed");
            }

            model::AudioGenerationResult result;
            result.wav_bytes.resize(wave.size());
            std::memcpy(result.wav_bytes.data(), wave.data(), wave.size());
            result.seed = ace_request.seed;
            result.output_audio_seconds =
                static_cast<double>(audio.n_samples) /
                static_cast<double>(audio.sample_rate);
            result.duration_ms =
                std::chrono::duration<float, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
            state.progress = 100;
            if (!state.report()) {
                return foundation::Err<model::AudioGenerationResult>(
                    foundation::ErrorCode::Cancelled,
                    "audio generation cancelled");
            }
            return foundation::Ok(std::move(result));
        } catch (const std::exception& error) {
            return foundation::Err<model::AudioGenerationResult>(
                foundation::ErrorCode::Internal,
                std::string("ACE-Step generation failed: ") + error.what());
        } catch (...) {
            return foundation::Err<model::AudioGenerationResult>(
                foundation::ErrorCode::Internal,
                "ACE-Step generation failed");
        }
    }

private:
    void release_context() noexcept {
        if (context_) {
            ace_synth_free(context_);
        }
        context_ = nullptr;
        if (store_) {
            store_free(store_);
        }
        store_ = nullptr;
        retained_vram_bytes_.store(0, std::memory_order_relaxed);
    }

    ModelStore* store_{nullptr};
    AceSynth* context_{nullptr};
    std::atomic<std::size_t> retained_vram_bytes_{0};
    std::string text_encoder_path_;
    std::string dit_path_;
    std::string vae_path_;
    std::string adapter_path_;
};

}

std::unique_ptr<model::IBackend> make_ace_step_backend(
    const model::ModelInfo& info) {
    return std::make_unique<AceStepBackend>(info);
}

}
