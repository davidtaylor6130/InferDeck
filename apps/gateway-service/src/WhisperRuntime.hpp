#pragma once

#include "Server.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>

namespace inferdeck::gateway {

struct WhisperResult {
    bool ok = false;
    std::string text;
    std::string language;
    std::string model;
    std::string error_code;
    std::string error_message;
    double duration_seconds = 0.0;
};

class WhisperRuntime {
public:
    static WhisperRuntime& Get();

    void Configure(const ServerConfig& config);
    bool Start();
    void Stop();
    bool Restart();
    bool LoadModel(const std::string& model);
    std::size_t RescanModels();

    WhisperResult Transcribe(const std::string& audio_path, const nlohmann::json& params, bool translate);
    nlohmann::json StatusJson() const;
    nlohmann::json ActivityJson() const;
    nlohmann::json ModelsJson() const;

private:
    WhisperRuntime() = default;

    std::string ResolveModelPath(const std::string& requested) const;
    std::string NormalizeModelId(const std::string& value) const;
    std::string Quote(const std::string& value) const;
    std::string CommandForRuntimeDirectory(const std::string& executable, const std::string& command) const;
    bool IsConfiguredLocked() const;
    nlohmann::json ActivityJsonLocked() const;

    mutable std::mutex mutex_;
    ServerConfig config_;
    bool configured_ = false;
    bool running_ = false;
    std::string current_model_;
    std::string last_error_;
    std::uint64_t queued_ = 0;
    std::uint64_t running_jobs_ = 0;
    std::uint64_t completed_ = 0;
    std::uint64_t failed_ = 0;
    std::string last_text_;
    double last_duration_seconds_ = 0.0;
};

} // namespace inferdeck::gateway
