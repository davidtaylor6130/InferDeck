/// @file ITtsBackend.hpp
/// @brief Text-to-speech backend interface.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>
#include "backends/ITextBackend.hpp"

namespace inferdeck::backends {

struct TTSSegment {
    std::string text;
    int start_ms;
    int end_ms;
    float pitch;
    float speed;
};

class ITtsBackend : public ITextBackend {
public:
    ~ITtsBackend() override = default;

    virtual std::vector<uint8_t> Synthesize(const std::string& text,
                                             const nlohmann::json& params = nlohmann::json::object()) = 0;

    virtual nlohmann::json GetAvailableVoices() const = 0;
    virtual bool SetVoice(const std::string& voice_id) = 0;
    virtual std::string GetVoiceId() const = 0;
    virtual std::vector<TTSSegment> SegmentText(const std::string& text) = 0;
};

} // namespace inferdeck::backends
