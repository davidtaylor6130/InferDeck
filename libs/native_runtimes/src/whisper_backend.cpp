#include "model/fixed_backend.hpp"
#include "model/imodel.hpp"

#include <whisper.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace inferdeck::native_runtimes {

namespace {

model::ModelInfo transcription_info(model::ModelInfo info) {
    info.runtime = "whisper_cpp";
    info.modality = "audio_transcription";
    info.capabilities = {"audio_transcription"};
    info.n_slots = 1;
    info.min_slots = 1;
    return info;
}

std::vector<float> resample(const std::vector<float>& input, int input_rate) {
    if (input_rate == WHISPER_SAMPLE_RATE) return input;
    const std::size_t count = static_cast<std::size_t>(
        std::ceil(static_cast<double>(input.size()) * WHISPER_SAMPLE_RATE / input_rate));
    std::vector<float> output(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double source = static_cast<double>(index) * input_rate / WHISPER_SAMPLE_RATE;
        const std::size_t left = std::min(static_cast<std::size_t>(source), input.size() - 1);
        const std::size_t right = std::min(left + 1, input.size() - 1);
        const float fraction = static_cast<float>(source - left);
        output[index] = input[left] + (input[right] - input[left]) * fraction;
    }
    return output;
}

struct CallbackState {
    const std::function<bool(int)>* progress{};
    std::atomic<bool> cancelled{false};
};

void on_progress(whisper_context*, whisper_state*, int progress, void* data) {
    auto* state = static_cast<CallbackState*>(data);
    if (state->progress && *state->progress && !(*state->progress)(progress)) state->cancelled.store(true);
}

bool should_abort(void* data) {
    return static_cast<CallbackState*>(data)->cancelled.load();
}

class WhisperBackend final : public model::FixedBackend, public model::ITranscriptionBackend {
public:
    explicit WhisperBackend(model::ModelInfo info)
        : FixedBackend(transcription_info(std::move(info))),
          model_path_(info_.artifacts.contains("model") ? info_.artifacts.at("model") : info_.gguf_path) {}

    ~WhisperBackend() override { if (context_) whisper_free(context_); }

    foundation::Result<void> load() override {
        if (model_path_.empty()) return foundation::Err<void>(foundation::ErrorCode::InvalidArgument, "whisper.cpp model artifact is missing");
        auto params = whisper_context_default_params();
        params.use_gpu = true;
        params.flash_attn = true;
        context_ = whisper_init_from_file_with_params(model_path_.c_str(), params);
        if (!context_) return foundation::Err<void>(foundation::ErrorCode::Unavailable, "whisper.cpp failed to load model");
        set_loaded(true);
        return foundation::Ok();
    }

    foundation::Result<void> unload() override {
        set_loaded(false);
        if (context_) whisper_free(context_);
        context_ = nullptr;
        return foundation::Ok();
    }

    foundation::Result<model::TranscriptionResult> transcribe(
        int, const model::TranscriptionRequest& request,
        const std::function<bool(int)>& progress) override {
        if (!context_) return foundation::Err<model::TranscriptionResult>(foundation::ErrorCode::NotLoaded, "transcription model is not loaded");
        if (!request.language.empty() && whisper_lang_id(request.language.c_str()) < 0) {
            return foundation::Err<model::TranscriptionResult>(foundation::ErrorCode::InvalidArgument, "unsupported transcription language");
        }
        auto pcm = resample(request.pcm, request.sample_rate);
        auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.n_threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
        params.language = request.language.empty() ? nullptr : request.language.c_str();
        params.initial_prompt = request.prompt.empty() ? nullptr : request.prompt.c_str();
        params.temperature = request.temperature;
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        CallbackState state{&progress};
        params.progress_callback = on_progress;
        params.progress_callback_user_data = &state;
        params.abort_callback = should_abort;
        params.abort_callback_user_data = &state;
        const auto started = std::chrono::steady_clock::now();
        if (whisper_full(context_, params, pcm.data(), static_cast<int>(pcm.size())) != 0) {
            return foundation::Err<model::TranscriptionResult>(
                state.cancelled.load() ? foundation::ErrorCode::Cancelled : foundation::ErrorCode::Internal,
                state.cancelled.load() ? "transcription cancelled" : "whisper.cpp transcription failed");
        }
        model::TranscriptionResult result;
        const int segments = whisper_full_n_segments(context_);
        for (int index = 0; index < segments; ++index) {
            const char* text = whisper_full_get_segment_text(context_, index);
            if (text) result.text += text;
        }
        const int language_id = whisper_full_lang_id(context_);
        const char* language = language_id >= 0 ? whisper_lang_str(language_id) : nullptr;
        result.language = language ? language : request.language;
        result.duration_seconds = static_cast<float>(pcm.size()) / WHISPER_SAMPLE_RATE;
        result.inference_ms = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return foundation::Ok(std::move(result));
    }

private:
    whisper_context* context_{nullptr};
    std::string model_path_;
};

}

std::unique_ptr<model::IBackend> make_whisper_backend(const model::ModelInfo& info) {
    return std::make_unique<WhisperBackend>(info);
}

}
