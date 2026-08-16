#include "model/fixed_backend.hpp"
#include "model/imodel.hpp"

#include <whisper.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <filesystem>
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

std::string artifact(const model::ModelInfo& info, const std::string& key) {
    const auto found = info.artifacts.find(key);
    return found == info.artifacts.end() ? std::string{} : found->second;
}

foundation::Result<int> thread_count(const model::ModelInfo& info) {
    const auto configured = artifact(info, "num_threads");
    if (configured.empty()) {
        return foundation::Ok(static_cast<int>(
            std::min(4U, std::max(1U, std::thread::hardware_concurrency()))));
    }
    int value = 0;
    const auto [end, error] = std::from_chars(
        configured.data(), configured.data() + configured.size(), value);
    if (error != std::errc{} || end != configured.data() + configured.size() ||
        value < 1 || value > 64) {
        return foundation::Err<int>(
            foundation::ErrorCode::InvalidArgument,
            "whisper.cpp num_threads must be an integer between 1 and 64");
    }
    return foundation::Ok(value);
}

std::vector<float> resample(const std::vector<float>& input, int input_rate) {
    if (input.empty() || input_rate <= 0) return {};
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
    if (!state->progress || !*state->progress) return;
    try {
        if (!(*state->progress)(progress)) state->cancelled.store(true);
    } catch (...) {
        state->cancelled.store(true);
    }
}

bool should_abort(void* data) {
    return static_cast<CallbackState*>(data)->cancelled.load();
}

class WhisperBackend final : public model::FixedBackend, public model::ITranscriptionBackend {
public:
    explicit WhisperBackend(model::ModelInfo info)
        : FixedBackend(transcription_info(std::move(info))),
          model_path_(info_.artifacts.contains("model") ? info_.artifacts.at("model") : info_.gguf_path),
          provider_(artifact(info_, "provider")) {
        if (provider_.empty()) provider_ = "cpu";
    }

    ~WhisperBackend() override { release_context(); }

    foundation::Result<void> load() override {
        set_loaded(false);
        release_context();
        if (model_path_.empty()) return foundation::Err<void>(foundation::ErrorCode::InvalidArgument, "whisper.cpp model artifact is missing");
        if (provider_ != "cpu" && provider_ != "gpu" && provider_ != "auto") {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         "whisper.cpp provider must be cpu, gpu, or auto");
        }
        if (provider_ != "cpu" && info_.vram_required_mb <= 0) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument,
                                         "whisper.cpp GPU providers require positive VRAM accounting");
        }
        if (!std::filesystem::is_regular_file(model_path_)) {
            return foundation::Err<void>(foundation::ErrorCode::NotFound, "whisper.cpp model artifact was not found: " + model_path_);
        }
        auto threads = thread_count(info_);
        if (!threads) return foundation::Err<void>(threads.error().code, threads.error().message);
        n_threads_ = *threads;
        auto params = whisper_context_default_params();
        params.use_gpu = provider_ != "cpu";
        params.flash_attn = params.use_gpu;
        context_ = whisper_init_from_file_with_params(model_path_.c_str(), params);
        if (!context_ && provider_ == "auto") {
            params.use_gpu = false;
            params.flash_attn = false;
            context_ = whisper_init_from_file_with_params(model_path_.c_str(), params);
        }
        if (!context_) return foundation::Err<void>(foundation::ErrorCode::Unavailable, "whisper.cpp failed to load model");
        set_loaded(true);
        return foundation::Ok();
    }

    foundation::Result<void> unload() override {
        set_loaded(false);
        release_context();
        return foundation::Ok();
    }

    foundation::Result<model::TranscriptionResult> transcribe(
        int, const model::TranscriptionRequest& request,
        const std::function<bool(int)>& progress) override {
        if (!context_) return foundation::Err<model::TranscriptionResult>(foundation::ErrorCode::NotLoaded, "transcription model is not loaded");
        if (request.pcm.empty() || request.sample_rate < 8000 || request.sample_rate > 192000 ||
            request.pcm.size() > static_cast<std::size_t>(request.sample_rate) * 60ULL * 30ULL) {
            return foundation::Err<model::TranscriptionResult>(foundation::ErrorCode::InvalidArgument, "audio duration is invalid or exceeds 30 minutes");
        }
        if (!std::isfinite(request.temperature) || request.temperature < 0.0f || request.temperature > 1.0f) {
            return foundation::Err<model::TranscriptionResult>(foundation::ErrorCode::InvalidArgument, "transcription temperature must be finite and between 0 and 1");
        }
        if (std::any_of(request.pcm.begin(), request.pcm.end(), [](float sample) { return !std::isfinite(sample); })) {
            return foundation::Err<model::TranscriptionResult>(foundation::ErrorCode::InvalidArgument, "audio samples must be finite");
        }
        std::string language = request.language;
        if (language.empty() && !whisper_is_multilingual(context_)) language = "en";
        if (!language.empty() && language != "auto" && whisper_lang_id(language.c_str()) < 0) {
            return foundation::Err<model::TranscriptionResult>(foundation::ErrorCode::InvalidArgument, "unsupported transcription language");
        }
        auto pcm = resample(request.pcm, request.sample_rate);
        auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.n_threads = n_threads_;
        params.language = language.empty() || language == "auto" ? nullptr : language.c_str();
        params.detect_language = language.empty() || language == "auto";
        params.initial_prompt = request.prompt.empty() ? nullptr : request.prompt.c_str();
        params.temperature = request.temperature;
        params.no_context = true;
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        CallbackState state{&progress};
        params.progress_callback = on_progress;
        params.progress_callback_user_data = &state;
        params.abort_callback = should_abort;
        params.abort_callback_user_data = &state;
        const auto started = std::chrono::steady_clock::now();
        const int transcription_status =
            whisper_full(context_, params, pcm.data(), static_cast<int>(pcm.size()));
        if (state.cancelled.load() || transcription_status != 0) {
            return foundation::Err<model::TranscriptionResult>(
                state.cancelled.load() ? foundation::ErrorCode::Cancelled : foundation::ErrorCode::Internal,
                state.cancelled.load() ? "transcription cancelled" : "whisper.cpp transcription failed");
        }
        model::TranscriptionResult result;
        const int segments = whisper_full_n_segments(context_);
        for (int index = 0; index < segments; ++index) {
            const char* text = whisper_full_get_segment_text(context_, index);
            if (text) result.text += text;
            model::TranscriptionSegment segment;
            segment.id = index;
            segment.start_seconds = static_cast<float>(whisper_full_get_segment_t0(context_, index)) / 100.0f;
            segment.end_seconds = static_cast<float>(whisper_full_get_segment_t1(context_, index)) / 100.0f;
            segment.text = text ? text : "";
            segment.no_speech_probability = whisper_full_get_segment_no_speech_prob(context_, index);
            const int tokens = whisper_full_n_tokens(context_, index);
            double log_probability = 0.0;
            int probability_count = 0;
            segment.tokens.reserve(static_cast<std::size_t>(std::max(0, tokens)));
            for (int token = 0; token < tokens; ++token) {
                segment.tokens.push_back(whisper_full_get_token_id(context_, index, token));
                const float probability = whisper_full_get_token_p(context_, index, token);
                if (probability > 0.0f) {
                    log_probability += std::log(probability);
                    ++probability_count;
                }
            }
            if (probability_count > 0) {
                segment.avg_logprob = static_cast<float>(log_probability / probability_count);
            }
            result.segments.push_back(std::move(segment));
        }
        const int language_id = whisper_full_lang_id(context_);
        const char* detected_language = language_id >= 0 ? whisper_lang_str(language_id) : nullptr;
        result.language = detected_language ? detected_language : language;
        result.duration_seconds = static_cast<float>(pcm.size()) / WHISPER_SAMPLE_RATE;
        result.inference_ms = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return foundation::Ok(std::move(result));
    }

private:
    void release_context() noexcept {
        if (context_) whisper_free(context_);
        context_ = nullptr;
    }

    whisper_context* context_{nullptr};
    std::string model_path_;
    std::string provider_;
    int n_threads_{1};
};

}

std::unique_ptr<model::IBackend> make_whisper_backend(const model::ModelInfo& info) {
    return std::make_unique<WhisperBackend>(info);
}

}
