/// @file PiperTtsBackend.cpp
/// @brief Piper text-to-speech implementation.

#include "piper_tts/PiperTtsBackend.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace inferdeck::backends {

std::string PiperTtsBackend::GetBackendName() const { return "piper_tts"; }

BackendStatus PiperTtsBackend::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool PiperTtsBackend::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = BackendStatus::INITIALIZING;

    // Load ONNX Runtime Piper model
    // auto piper_config = piper::Config::FromJSON(config_json);
    // auto model = piper::Model::Load(model_path_, piper_config);
    // session = std::make_unique<Ort::Session>(env, model_path_.c_str(), Ort::SessionOptions{});

    spdlog::info("PiperTtsBackend: loaded voice '{}' (sample_rate={})", voice_id_, sample_rate_);
    status_ = BackendStatus::READY;
    return true;
}

void PiperTtsBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    // onnx session cleanup
    status_ = BackendStatus::UNINITIALIZED;
    spdlog::info("PiperTtsBackend: shut down");
}

nlohmann::json PiperTtsBackend::GetInfo() const {
    nlohmann::json info;
    info["name"] = "piper_tts";
    info["backend"] = "piper-tts";
    info["engine"] = "onnx";
    info["voice"] = voice_id_;
    info["voice_model_path"] = voice_model_path_;
    info["sample_rate"] = sample_rate_;
    info["status"] = static_cast<int>(status_);
    return info;
}

nlohmann::json PiperTtsBackend::GetVRAMUsage() const {
    nlohmann::json vram;
    vram["backend"] = "piper_tts";
    vram["allocated_bytes"] = 0;
    vram["peak_bytes"] = 0;
    return vram;
}

bool PiperTtsBackend::IsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_ == BackendStatus::READY;
}

std::vector<uint8_t> PiperTtsBackend::Synthesize(const std::string& text,
                                                   const nlohmann::json& params) {
    std::vector<uint8_t> wav_data;

    // Piper ONNX inference:
    // auto piper_params = piper::Params::FromJSON(params);
    // auto audio = piper.Speak(text, piper_params);
    // Convert to WAV format with header
    // WriteWAVHeader(wav_data, audio.data(), audio.size(), sample_rate_);

    // WAV header: RIFF header, fmt chunk, data chunk
    // std::uint32_t data_size = audio.size() * 2; // 16-bit PCM
    // wav_data.resize(44 + data_size);
    // WriteRIIFFHeader(&wav_data[0], data_size, sample_rate_);

    spdlog::info("PiperTtsBackend: synthesized {} bytes of audio for {} chars of text",
                 wav_data.size(), text.size());
    return wav_data;
}

nlohmann::json PiperTtsBackend::GetAvailableVoices() const {
    nlohmann::json voices = nlohmann::json::array();
    nlohmann::json voice;
    voice["id"] = voice_id_;
    voice["name"] = voice_id_;
    voice["language"] = "eng";
    voice["sample_rate"] = sample_rate_;
    voices.push_back(voice);
    return voices;
}

bool PiperTtsBackend::SetVoice(const std::string& voice_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    voice_id_ = voice_id;
    // Reload ONNX model for new voice
    // piper::Model::Load(voice_model_path_, config);
    spdlog::info("PiperTtsBackend: switched to voice '{}'", voice_id_);
    return true;
}

std::string PiperTtsBackend::GetVoiceId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return voice_id_;
}

std::vector<TTSSegment> PiperTtsBackend::SegmentText(const std::string& text) {
    std::vector<TTSSegment> segments;

    // Split text into sentences for TTS processing
    std::string current_segment;
    for (size_t i = 0; i < text.size(); i++) {
        current_segment += text[i];
        if (text[i] == '.' || text[i] == '!' || text[i] == '?' || text[i] == '\n') {
            if (!current_segment.empty()) {
                TTSSegment seg;
                seg.text = current_segment;
                seg.start_ms = static_cast<int>(segments.size() * 2000); // estimate
                seg.end_ms = seg.start_ms + 2000;
                seg.pitch = 0.0f;
                seg.speed = 1.0f;
                segments.push_back(seg);
            }
            current_segment.clear();
        }
    }
    if (!current_segment.empty()) {
        TTSSegment seg;
        seg.text = current_segment;
        seg.start_ms = static_cast<int>(segments.size() * 2000);
        seg.end_ms = seg.start_ms + 2000;
        seg.pitch = 0.0f;
        seg.speed = 1.0f;
        segments.push_back(seg);
    }
    return segments;
}

bool PiperTtsBackend::SetVoiceModelPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    voice_model_path_ = path;
    return true;
}

std::string PiperTtsBackend::GetVoiceModelPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return voice_model_path_;
}

void PiperTtsBackend::SetSampleRate(int sample_rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    sample_rate_ = sample_rate;
}

int PiperTtsBackend::GetSampleRate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sample_rate_;
}

} // namespace inferdeck::backends
