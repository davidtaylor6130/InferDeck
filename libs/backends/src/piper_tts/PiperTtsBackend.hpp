/// @file PiperTtsBackend.hpp
/// @brief Piper text-to-speech backend with swappable voice models.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include "backends/ITtsBackend.hpp"

namespace inferdeck::backends {

class PiperTtsBackend : public ITtsBackend {
public:
    PiperTtsBackend() = default;
    ~PiperTtsBackend() override = default;

    std::string GetBackendName() const override;
    BackendStatus GetStatus() const override;
    bool Initialize() override;
    void Shutdown() override;
    nlohmann::json GetInfo() const override;
    nlohmann::json GetVRAMUsage() const override;
    bool IsReady() const override;

    std::vector<uint8_t> Synthesize(const std::string& text,
                                     const nlohmann::json& params = nlohmann::json::object()) override;
    nlohmann::json GetAvailableVoices() const override;
    bool SetVoice(const std::string& voice_id) override;
    std::string GetVoiceId() const override;
    std::vector<TTSSegment> SegmentText(const std::string& text) override;

    // Configuration
    bool SetVoiceModelPath(const std::string& path);
    std::string GetVoiceModelPath() const;
    void SetSampleRate(int sample_rate);
    int GetSampleRate() const;

private:
    mutable std::mutex mutex_;
    std::string voice_model_path_;
    std::string voice_id_;
    BackendStatus status_{BackendStatus::UNINITIALIZED};
    int sample_rate_{22050};
    std::unordered_map<std::string, std::string> available_voices_;
};

} // namespace inferdeck::backends
