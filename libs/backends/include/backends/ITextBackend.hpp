/// @file ITextBackend.hpp
/// @brief Base interface for text-based backends (chat, completion, text gen).

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

namespace inferdeck::backends {

enum class BackendStatus {
    UNINITIALIZED,
    INITIALIZING,
    READY,
    BUSY,
    ERROR
};

struct TextGenResult {
    std::string text;
    int prompt_tokens;
    int completion_tokens;
    double inference_time_ms;
    std::vector<float> logits;
    nlohmann::json metadata;
};

class ITextBackend {
public:
    virtual ~ITextBackend() = default;

    virtual std::string GetBackendName() const = 0;
    virtual BackendStatus GetStatus() const = 0;
    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
    virtual nlohmann::json GetInfo() const = 0;
    virtual nlohmann::json GetVRAMUsage() const = 0;
    virtual bool IsReady() const = 0;
};

} // namespace inferdeck::backends
