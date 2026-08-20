#include "model/fixed_backend.hpp"
#include "model/imodel.hpp"

#include <sherpa-onnx/c-api/c-api.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace inferdeck::native_runtimes {

namespace {

model::ModelInfo speech_info(model::ModelInfo info) {
    info.runtime = "sherpa_onnx";
    info.modality = "audio_speech";
    info.capabilities = {"audio_speech"};
    info.n_slots = 1;
    info.min_slots = 1;
    return info;
}

std::string artifact(const model::ModelInfo& info, const std::string& key) {
    const auto found = info.artifacts.find(key);
    return found == info.artifacts.end() ? std::string{} : found->second;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
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

foundation::Result<int> speaker_id(std::string voice, int speakers) {
    voice = lower(std::move(voice));
    static constexpr std::array openai_aliases{
        std::pair{"alloy", 0},
        std::pair{"ash", 2},     std::pair{"ballad", 1},
        std::pair{"coral", 7},   std::pair{"echo", 3},
        std::pair{"fable", 4},   std::pair{"nova", 5},
        std::pair{"onyx", 0},    std::pair{"sage", 8},
        std::pair{"shimmer", 6}, std::pair{"verse", 9},
        std::pair{"marin", 10},  std::pair{"cedar", 11},
    };
    static constexpr std::array legacy_aliases{
        std::pair{"default", 0},
        std::pair{"m1", 0},      std::pair{"m2", 1},
        std::pair{"m3", 2},      std::pair{"m4", 3},
        std::pair{"m5", 4},      std::pair{"f1", 5},
        std::pair{"f2", 6},      std::pair{"f3", 7},
        std::pair{"f4", 8},      std::pair{"f5", 9},
    };
    int value = -1;
    const auto openai_alias = std::find_if(
        openai_aliases.begin(), openai_aliases.end(),
        [&voice](const auto& item) { return item.first == voice; });
    if (openai_alias != openai_aliases.end()) {
        value = speakers == 1 ? 0 : openai_alias->second;
    } else {
        const auto legacy_alias = std::find_if(
            legacy_aliases.begin(), legacy_aliases.end(),
            [&voice](const auto& item) { return item.first == voice; });
        if (legacy_alias != legacy_aliases.end()) {
            value = legacy_alias->second;
        } else {
            const auto [end, error] =
                std::from_chars(voice.data(), voice.data() + voice.size(), value);
            if (error != std::errc{} || end != voice.data() + voice.size()) {
                return foundation::Err<int>(
                    foundation::ErrorCode::InvalidArgument,
                    "voice must be an OpenAI voice alias, M1-M5, F1-F5, or a numeric speaker id");
            }
        }
    }
    if (speakers < 1 || value < 0 || value >= speakers) {
        return foundation::Err<int>(
            foundation::ErrorCode::InvalidArgument,
            "voice speaker id is out of range for this speech model");
    }
    return foundation::Ok(value);
}

foundation::Result<int> configured_speaker_count(
    const model::ModelInfo& info, const std::string& engine) {
    if (engine == "supertonic") return foundation::Ok(1);
    const auto configured = artifact(info, "speakers");
    if (configured.empty()) return foundation::Ok(1);
    int value = 0;
    const auto [end, error] = std::from_chars(
        configured.data(), configured.data() + configured.size(), value);
    if (error != std::errc{} || end != configured.data() + configured.size() ||
        value < 1 || value > 100'000) {
        return foundation::Err<int>(
            foundation::ErrorCode::InvalidArgument,
            "sherpa-onnx speakers must be an integer between 1 and 100000");
    }
    return foundation::Ok(value);
}

void append16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xff));
    output.push_back(static_cast<std::byte>((value >> 8) & 0xff));
}

void append32(std::vector<std::byte>& output, std::uint32_t value) {
    append16(output, static_cast<std::uint16_t>(value & 0xffff));
    append16(output, static_cast<std::uint16_t>(value >> 16));
}

std::vector<std::byte> pcm16(const float* samples, std::size_t count) {
    std::vector<std::byte> output;
    output.reserve(count * 2);
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = static_cast<std::int16_t>(std::lrint(std::clamp(samples[index], -1.0f, 1.0f) * 32767.0f));
        append16(output, static_cast<std::uint16_t>(value));
    }
    return output;
}

std::vector<std::byte> wave(const float* samples, std::size_t count, int sample_rate) {
    auto pcm = pcm16(samples, count);
    std::vector<std::byte> output;
    output.reserve(44 + pcm.size());
    for (char value : std::string("RIFF")) output.push_back(static_cast<std::byte>(value));
    append32(output, static_cast<std::uint32_t>(36 + pcm.size()));
    for (char value : std::string("WAVEfmt ")) output.push_back(static_cast<std::byte>(value));
    append32(output, 16);
    append16(output, 1);
    append16(output, 1);
    append32(output, static_cast<std::uint32_t>(sample_rate));
    append32(output, static_cast<std::uint32_t>(sample_rate * 2));
    append16(output, 2);
    append16(output, 16);
    for (char value : std::string("data")) output.push_back(static_cast<std::byte>(value));
    append32(output, static_cast<std::uint32_t>(pcm.size()));
    output.insert(output.end(), pcm.begin(), pcm.end());
    return output;
}

struct StreamState {
    const std::function<bool(const std::byte*, std::size_t)>* stream{};
    bool raw_pcm{false};
    bool cancelled{false};
};

int32_t audio_callback(const float* samples, int32_t count, float, void* data) {
    auto* state = static_cast<StreamState*>(data);
    try {
        if (!state || count < 0 || (count > 0 && !samples)) return 0;
        if (!state->stream || !*state->stream) return 1;
        if (!state->raw_pcm) {
            if (!(*state->stream)(nullptr, 0)) {
                state->cancelled = true;
                return 0;
            }
            return 1;
        }
        auto bytes = pcm16(samples, static_cast<std::size_t>(count));
        if (!(*state->stream)(bytes.data(), bytes.size())) {
            state->cancelled = true;
            return 0;
        }
        return 1;
    } catch (...) {
        if (state) state->cancelled = true;
        return 0;
    }
}

class SherpaTtsBackend final : public model::FixedBackend, public model::ISpeechBackend {
public:
    explicit SherpaTtsBackend(model::ModelInfo info)
        : FixedBackend(speech_info(std::move(info))),
          engine_(lower(artifact(info_, "engine"))),
          model_path_(artifact(info_, "model")),
          tokens_(artifact(info_, "tokens")), lexicon_(artifact(info_, "lexicon")),
          data_dir_(artifact(info_, "data_dir")), dict_dir_(artifact(info_, "dict_dir")),
          duration_predictor_(artifact(info_, "duration_predictor")),
          text_encoder_(artifact(info_, "text_encoder")),
          vector_estimator_(artifact(info_, "vector_estimator")),
          vocoder_(artifact(info_, "vocoder")),
          tts_json_(artifact(info_, "tts_json")),
          unicode_indexer_(artifact(info_, "unicode_indexer")),
          voice_style_(artifact(info_, "voice_style")),
          provider_(artifact(info_, "provider")) {
        if (model_path_.empty()) model_path_ = info_.gguf_path;
        if (engine_.empty()) {
            engine_ = duration_predictor_.empty() ? "vits" : "supertonic";
        }
        if (provider_.empty()) provider_ = "cpu";
    }

    ~SherpaTtsBackend() override { release_tts(); }

    foundation::Result<void> load() override {
        set_loaded(false);
        release_tts();
        auto speakers = configured_speaker_count(info_, engine_);
        if (!speakers) {
            return foundation::Err<void>(speakers.error().code,
                                         speakers.error().message);
        }
        auto threads = thread_count(info_);
        if (!threads) {
            return foundation::Err<void>(threads.error().code,
                                         threads.error().message);
        }
        if (provider_ != "cpu" && info_.vram_required_mb <= 0) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "sherpa-onnx GPU providers require positive VRAM accounting");
        }
        SherpaOnnxOfflineTtsConfig config{};
        if (engine_ == "supertonic") {
            if (duration_predictor_.empty() || text_encoder_.empty() ||
                vector_estimator_.empty() || vocoder_.empty() ||
                tts_json_.empty() || unicode_indexer_.empty() ||
                voice_style_.empty()) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "sherpa-onnx Supertonic requires duration_predictor, text_encoder, vector_estimator, vocoder, tts_json, unicode_indexer, and voice_style artifacts");
            }
            config.model.supertonic.duration_predictor =
                duration_predictor_.c_str();
            config.model.supertonic.text_encoder = text_encoder_.c_str();
            config.model.supertonic.vector_estimator =
                vector_estimator_.c_str();
            config.model.supertonic.vocoder = vocoder_.c_str();
            config.model.supertonic.tts_json = tts_json_.c_str();
            config.model.supertonic.unicode_indexer =
                unicode_indexer_.c_str();
            config.model.supertonic.voice_style = voice_style_.c_str();
        } else if (engine_ == "vits") {
            if (model_path_.empty() || tokens_.empty()) {
                return foundation::Err<void>(
                    foundation::ErrorCode::InvalidArgument,
                    "sherpa-onnx VITS model and tokens artifacts are required");
            }
            config.model.vits.model = model_path_.c_str();
            config.model.vits.tokens = tokens_.c_str();
            config.model.vits.lexicon = lexicon_.c_str();
            config.model.vits.data_dir = data_dir_.c_str();
            config.model.vits.dict_dir = dict_dir_.c_str();
            config.model.vits.noise_scale = 0.667f;
            config.model.vits.noise_scale_w = 0.8f;
            config.model.vits.length_scale = 1.0f;
        } else {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "unsupported sherpa-onnx TTS engine: " + engine_);
        }
        config.model.num_threads = *threads;
        config.model.provider = provider_.c_str();
        config.max_num_sentences = 1;
        config.silence_scale = 0.2f;
        tts_ = SherpaOnnxCreateOfflineTts(&config);
        if (!tts_) {
            return foundation::Err<void>(
                foundation::ErrorCode::Unavailable,
                "sherpa-onnx failed to load " + engine_ + " TTS model");
        }
        set_loaded(true);
        return foundation::Ok();
    }

    foundation::Result<void> unload() override {
        set_loaded(false);
        release_tts();
        return foundation::Ok();
    }

    foundation::Result<void> validate_speech_request(
        const model::SpeechRequest& request) override {
        if (request.format != "wav" && request.format != "pcm") {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "sherpa-onnx runtime supports wav and pcm responses");
        }
        if (!std::isfinite(request.speed) || request.speed < 0.25f ||
            request.speed > 4.0f) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "speech speed must be finite and between 0.25 and 4");
        }
        auto speakers = tts_
            ? foundation::Ok(SherpaOnnxOfflineTtsNumSpeakers(tts_))
            : configured_speaker_count(info_, engine_);
        if (!speakers) {
            return foundation::Err<void>(speakers.error().code,
                                         speakers.error().message);
        }
        auto speaker = speaker_id(request.voice, *speakers);
        if (!speaker) {
            return foundation::Err<void>(speaker.error().code,
                                         speaker.error().message);
        }
        return foundation::Ok();
    }

    foundation::Result<model::AudioResult> synthesize(
        int, const model::SpeechRequest& request,
        const std::function<bool(const std::byte*, std::size_t)>& stream) override {
        if (!tts_) return foundation::Err<model::AudioResult>(foundation::ErrorCode::NotLoaded, "speech model is not loaded");
        if (request.format != "wav" && request.format != "pcm") {
            return foundation::Err<model::AudioResult>(foundation::ErrorCode::InvalidArgument, "sherpa-onnx runtime supports wav and pcm responses");
        }
        if (!std::isfinite(request.speed) || request.speed < 0.25f || request.speed > 4.0f) {
            return foundation::Err<model::AudioResult>(foundation::ErrorCode::InvalidArgument, "speech speed must be finite and between 0.25 and 4");
        }
        auto speaker = speaker_id(
            request.voice, SherpaOnnxOfflineTtsNumSpeakers(tts_));
        if (!speaker) {
            return foundation::Err<model::AudioResult>(
                speaker.error().code, speaker.error().message);
        }
        SherpaOnnxGenerationConfig config{};
        config.sid = *speaker;
        config.speed = request.speed;
        config.silence_scale = 0.2f;
        StreamState state{&stream, request.format == "pcm", false};
        const auto started = std::chrono::steady_clock::now();
        const SherpaOnnxGeneratedAudio* audio = SherpaOnnxOfflineTtsGenerateWithConfig(
            tts_, request.input.c_str(), &config, audio_callback, &state);
        if (!audio || state.cancelled) {
            if (audio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
            return foundation::Err<model::AudioResult>(state.cancelled ? foundation::ErrorCode::Cancelled : foundation::ErrorCode::Internal,
                                                        state.cancelled ? "speech generation cancelled" : "sherpa-onnx synthesis failed");
        }
        model::AudioResult result;
        result.bytes = request.format == "wav" ? wave(audio->samples, audio->n, audio->sample_rate) :
                                                 pcm16(audio->samples, audio->n);
        result.content_type = request.format == "wav" ? "audio/wav" : "audio/pcm";
        result.duration_ms = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        return foundation::Ok(std::move(result));
    }

private:
    void release_tts() noexcept {
        if (tts_) SherpaOnnxDestroyOfflineTts(tts_);
        tts_ = nullptr;
    }

    const SherpaOnnxOfflineTts* tts_{nullptr};
    std::string engine_;
    std::string model_path_;
    std::string tokens_;
    std::string lexicon_;
    std::string data_dir_;
    std::string dict_dir_;
    std::string duration_predictor_;
    std::string text_encoder_;
    std::string vector_estimator_;
    std::string vocoder_;
    std::string tts_json_;
    std::string unicode_indexer_;
    std::string voice_style_;
    std::string provider_;
};

}

std::unique_ptr<model::IBackend> make_sherpa_tts_backend(const model::ModelInfo& info) {
    return std::make_unique<SherpaTtsBackend>(info);
}

}
