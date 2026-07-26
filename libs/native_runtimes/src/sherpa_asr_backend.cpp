#include "model/fixed_backend.hpp"
#include "model/imodel.hpp"

#include <sherpa-onnx/c-api/c-api.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace inferdeck::native_runtimes {

namespace {

model::ModelInfo transcription_info(model::ModelInfo info) {
    info.runtime = "sherpa_onnx";
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
        const auto available = std::max(1U, std::thread::hardware_concurrency());
        return foundation::Ok(static_cast<int>(std::min(4U, available)));
    }
    int value = 0;
    const auto [end, error] = std::from_chars(
        configured.data(), configured.data() + configured.size(), value);
    if (error != std::errc{} || end != configured.data() + configured.size() ||
        value < 1 || value > 64) {
        return foundation::Err<int>(
            foundation::ErrorCode::InvalidArgument,
            "sherpa-onnx num_threads must be an integer between 1 and 64");
    }
    return foundation::Ok(value);
}

foundation::Result<void> require_file(const std::string& value,
                                      const std::string& name) {
    if (value.empty()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "sherpa-onnx Parakeet requires the " + name + " artifact");
    }
    if (!std::filesystem::is_regular_file(value)) {
        return foundation::Err<void>(
            foundation::ErrorCode::NotFound,
            "sherpa-onnx Parakeet " + name + " artifact was not found: " +
                value);
    }
    return foundation::Ok();
}

class SherpaAsrBackend final : public model::FixedBackend,
                               public model::ITranscriptionBackend {
public:
    explicit SherpaAsrBackend(model::ModelInfo info)
        : FixedBackend(transcription_info(std::move(info))),
          encoder_(artifact(info_, "encoder")),
          decoder_(artifact(info_, "decoder")),
          joiner_(artifact(info_, "joiner")),
          tokens_(artifact(info_, "tokens")),
          provider_(artifact(info_, "provider")),
          model_type_(artifact(info_, "model_type")) {
        if (provider_.empty()) provider_ = "cpu";
        if (model_type_.empty()) model_type_ = "nemo_transducer";
    }

    ~SherpaAsrBackend() override { release_recognizer(); }

    foundation::Result<void> load() override {
        set_loaded(false);
        release_recognizer();

        for (const auto& item :
             {std::pair{&encoder_, "encoder"}, std::pair{&decoder_, "decoder"},
              std::pair{&joiner_, "joiner"}, std::pair{&tokens_, "tokens"}}) {
            auto checked = require_file(*item.first, item.second);
            if (!checked) return checked;
        }
        if (provider_ != "cpu") {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "sherpa-onnx Parakeet is restricted to the cpu provider so "
                "speech recognition cannot consume LLM VRAM");
        }
        auto threads = thread_count(info_);
        if (!threads) {
            return foundation::Err<void>(threads.error().code,
                                         threads.error().message);
        }

        SherpaOnnxOfflineRecognizerConfig config{};
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 80;
        config.model_config.transducer.encoder = encoder_.c_str();
        config.model_config.transducer.decoder = decoder_.c_str();
        config.model_config.transducer.joiner = joiner_.c_str();
        config.model_config.tokens = tokens_.c_str();
        config.model_config.num_threads = *threads;
        config.model_config.provider = provider_.c_str();
        config.model_config.model_type = model_type_.c_str();
        config.decoding_method = "greedy_search";
        config.max_active_paths = 4;

        recognizer_ = SherpaOnnxCreateOfflineRecognizer(&config);
        if (!recognizer_) {
            return foundation::Err<void>(
                foundation::ErrorCode::Unavailable,
                "sherpa-onnx failed to load the Parakeet ASR model");
        }
        set_loaded(true);
        return foundation::Ok();
    }

    foundation::Result<void> unload() override {
        set_loaded(false);
        release_recognizer();
        return foundation::Ok();
    }

    foundation::Result<model::TranscriptionResult> transcribe(
        int, const model::TranscriptionRequest& request,
        const std::function<bool(int)>& progress) override {
        if (!recognizer_) {
            return foundation::Err<model::TranscriptionResult>(
                foundation::ErrorCode::NotLoaded,
                "transcription model is not loaded");
        }
        if (request.pcm.empty() || request.sample_rate < 1 ||
            request.pcm.size() >
                static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
            return foundation::Err<model::TranscriptionResult>(
                foundation::ErrorCode::InvalidArgument,
                "audio samples and a positive sample rate are required");
        }
        if (std::any_of(request.pcm.begin(), request.pcm.end(),
                        [](float sample) { return !std::isfinite(sample); })) {
            return foundation::Err<model::TranscriptionResult>(
                foundation::ErrorCode::InvalidArgument,
                "audio samples must be finite");
        }
        if (progress && !progress(0)) {
            return foundation::Err<model::TranscriptionResult>(
                foundation::ErrorCode::Cancelled,
                "transcription cancelled");
        }

        const auto* stream = SherpaOnnxCreateOfflineStream(recognizer_);
        if (!stream) {
            return foundation::Err<model::TranscriptionResult>(
                foundation::ErrorCode::Internal,
                "sherpa-onnx failed to create a transcription stream");
        }
        struct StreamGuard {
            const SherpaOnnxOfflineStream* value;
            ~StreamGuard() {
                if (value) SherpaOnnxDestroyOfflineStream(value);
            }
        } stream_guard{stream};

        const auto started = std::chrono::steady_clock::now();
        SherpaOnnxAcceptWaveformOffline(
            stream, request.sample_rate, request.pcm.data(),
            static_cast<int32_t>(request.pcm.size()));
        SherpaOnnxDecodeOfflineStream(recognizer_, stream);
        const auto* recognized = SherpaOnnxGetOfflineStreamResult(stream);
        if (!recognized) {
            return foundation::Err<model::TranscriptionResult>(
                foundation::ErrorCode::Internal,
                "sherpa-onnx transcription failed");
        }
        struct ResultGuard {
            const SherpaOnnxOfflineRecognizerResult* value;
            ~ResultGuard() {
                if (value) SherpaOnnxDestroyOfflineRecognizerResult(value);
            }
        } result_guard{recognized};

        model::TranscriptionResult result;
        if (recognized->text) result.text = recognized->text;
        if (recognized->lang && *recognized->lang) {
            result.language = recognized->lang;
        } else if (!request.language.empty() && request.language != "auto") {
            result.language = request.language;
        }
        result.duration_seconds =
            static_cast<float>(request.pcm.size()) /
            static_cast<float>(request.sample_rate);
        result.inference_ms = std::chrono::duration<float, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();

        for (int32_t index = 0; index < recognized->segment_count; ++index) {
            model::TranscriptionSegment segment;
            segment.id = index;
            if (recognized->segment_timestamps) {
                segment.start_seconds =
                    recognized->segment_timestamps[index];
            }
            if (recognized->segment_durations) {
                segment.end_seconds =
                    segment.start_seconds +
                    recognized->segment_durations[index];
            }
            if (recognized->segment_texts_arr &&
                recognized->segment_texts_arr[index]) {
                segment.text = recognized->segment_texts_arr[index];
            }
            result.segments.push_back(std::move(segment));
        }
        if (result.segments.empty() && !result.text.empty()) {
            model::TranscriptionSegment segment;
            segment.id = 0;
            segment.end_seconds = result.duration_seconds;
            segment.text = result.text;
            result.segments.push_back(std::move(segment));
        }

        if (progress && !progress(100)) {
            return foundation::Err<model::TranscriptionResult>(
                foundation::ErrorCode::Cancelled,
                "transcription cancelled");
        }
        return foundation::Ok(std::move(result));
    }

private:
    void release_recognizer() noexcept {
        if (recognizer_) SherpaOnnxDestroyOfflineRecognizer(recognizer_);
        recognizer_ = nullptr;
    }

    const SherpaOnnxOfflineRecognizer* recognizer_{nullptr};
    std::string encoder_;
    std::string decoder_;
    std::string joiner_;
    std::string tokens_;
    std::string provider_;
    std::string model_type_;
};

} // namespace

std::unique_ptr<model::IBackend> make_sherpa_asr_backend(
    const model::ModelInfo& info) {
    return std::make_unique<SherpaAsrBackend>(info);
}

} // namespace inferdeck::native_runtimes
