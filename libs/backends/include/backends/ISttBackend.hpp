/// @file ISttBackend.hpp
/// @brief Speech-to-text backend interface.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>
#include "backends/ITextBackend.hpp"

namespace inferdeck::backends {

struct STTResult {
    std::string text;
    std::string language;
    double duration_seconds;
    double confidence;
    std::vector<float> timestamps;
    nlohmann::json metadata;
};

class ISttBackend : public ITextBackend {
public:
    ~ISttBackend() override = default;

    virtual STTResult Transcribe(const std::string& audio_path,
                                  const nlohmann::json& params = nlohmann::json::object()) = 0;

    virtual STTResult Translate(const std::string& audio_path,
                                 const nlohmann::json& params = nlohmann::json::object()) = 0;

    virtual nlohmann::json GetAvailableLanguages() const = 0;
};

} // namespace inferdeck::backends
