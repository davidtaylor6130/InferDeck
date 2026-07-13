#include "model/fixed_backend.hpp"
#include "model/imodel.hpp"

#include <sherpa-onnx/c-api/c-api.h>

#include <algorithm>
#include <chrono>
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
}

class SherpaTtsBackend final : public model::FixedBackend, public model::ISpeechBackend {
public:
    explicit SherpaTtsBackend(model::ModelInfo info)
        : FixedBackend(speech_info(std::move(info))), model_path_(artifact(info_, "model")),
          tokens_(artifact(info_, "tokens")), lexicon_(artifact(info_, "lexicon")),
          data_dir_(artifact(info_, "data_dir")), dict_dir_(artifact(info_, "dict_dir")),
          provider_(artifact(info_, "provider")) {
        if (model_path_.empty()) model_path_ = info_.gguf_path;
        if (provider_.empty()) provider_ = "cpu";
    }

    ~SherpaTtsBackend() override { if (tts_) SherpaOnnxDestroyOfflineTts(tts_); }

    foundation::Result<void> load() override {
        if (model_path_.empty() || tokens_.empty()) {
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument, "sherpa-onnx VITS model and tokens artifacts are required");
        }
        SherpaOnnxOfflineTtsConfig config{};
        config.model.vits.model = model_path_.c_str();
        config.model.vits.tokens = tokens_.c_str();
        config.model.vits.lexicon = lexicon_.c_str();
        config.model.vits.data_dir = data_dir_.c_str();
        config.model.vits.dict_dir = dict_dir_.c_str();
        config.model.vits.noise_scale = 0.667f;
        config.model.vits.noise_scale_w = 0.8f;
        config.model.vits.length_scale = 1.0f;
        config.model.num_threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
        config.model.provider = provider_.c_str();
        config.max_num_sentences = 1;
        tts_ = SherpaOnnxCreateOfflineTts(&config);
        if (!tts_) return foundation::Err<void>(foundation::ErrorCode::Unavailable, "sherpa-onnx failed to load TTS model");
        set_loaded(true);
        return foundation::Ok();
    }

    foundation::Result<void> unload() override {
        set_loaded(false);
        if (tts_) SherpaOnnxDestroyOfflineTts(tts_);
        tts_ = nullptr;
        return foundation::Ok();
    }

    foundation::Result<model::AudioResult> synthesize(
        int, const model::SpeechRequest& request,
        const std::function<bool(const std::byte*, std::size_t)>& stream) override {
        if (!tts_) return foundation::Err<model::AudioResult>(foundation::ErrorCode::NotLoaded, "speech model is not loaded");
        if (request.format != "wav" && request.format != "pcm") {
            return foundation::Err<model::AudioResult>(foundation::ErrorCode::InvalidArgument, "sherpa-onnx runtime supports wav and pcm responses");
        }
        int speaker = 0;
        if (request.voice != "default") {
            try { speaker = std::stoi(request.voice); }
            catch (...) { return foundation::Err<model::AudioResult>(foundation::ErrorCode::InvalidArgument, "voice must be default or a numeric speaker id"); }
        }
        if (speaker < 0 || speaker >= SherpaOnnxOfflineTtsNumSpeakers(tts_)) {
            return foundation::Err<model::AudioResult>(foundation::ErrorCode::InvalidArgument, "voice speaker id is out of range");
        }
        SherpaOnnxGenerationConfig config{};
        config.sid = speaker;
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
    const SherpaOnnxOfflineTts* tts_{nullptr};
    std::string model_path_;
    std::string tokens_;
    std::string lexicon_;
    std::string data_dir_;
    std::string dict_dir_;
    std::string provider_;
};

}

std::unique_ptr<model::IBackend> make_sherpa_tts_backend(const model::ModelInfo& info) {
    return std::make_unique<SherpaTtsBackend>(info);
}

}
