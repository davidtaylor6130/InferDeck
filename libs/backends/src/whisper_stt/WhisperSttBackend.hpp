/// @file WhisperSttBackend.hpp
/// @brief Whisper GGML-based speech-to-text backend.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include "backends/ISttBackend.hpp"

namespace inferdeck::backends {

class WhisperSttBackend : public ISttBackend {
public:
    WhisperSttBackend() = default;
    ~WhisperSttBackend() override = default;

    std::string GetBackendName() const override;
    BackendStatus GetStatus() const override;
    bool Initialize() override;
    void Shutdown() override;
    nlohmann::json GetInfo() const override;
    nlohmann::json GetVRAMUsage() const override;
    bool IsReady() const override;

    STTResult Transcribe(const std::string& audio_path,
                          const nlohmann::json& params = nlohmann::json::object()) override;
    STTResult Translate(const std::string& audio_path,
                         const nlohmann::json& params = nlohmann::json::object()) override;
    nlohmann::json GetAvailableLanguages() const override;

    // Configuration
    bool SetModelPath(const std::string& path);
    std::string GetModelPath() const;
    bool SetThreads(int threads);
    int GetThreads() const;

private:
    mutable std::mutex mutex_;
    std::string model_path_;
    std::string model_name_;
    BackendStatus status_{BackendStatus::UNINITIALIZED};
    int threads_{4};
    std::string detected_language_;
    bool language_autodetect_{true};
};

} // namespace inferdeck::backends
