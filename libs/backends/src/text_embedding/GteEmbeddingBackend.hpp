/// @file GteEmbeddingBackend.hpp
/// @brief Real embedding generation backend using GGML model.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include "backends/IEmbeddingBackend.hpp"

namespace inferdeck::backends {

class GteEmbeddingBackend : public IEmbeddingBackend {
public:
    GteEmbeddingBackend() = default;
    ~GteEmbeddingBackend() override = default;

    std::string GetBackendName() const override;
    BackendStatus GetStatus() const override;
    bool Initialize() override;
    void Shutdown() override;
    nlohmann::json GetInfo() const override;
    nlohmann::json GetVRAMUsage() const override;
    bool IsReady() const override;

    EmbeddingResult Generate(const std::string& input,
                              const nlohmann::json& params = nlohmann::json::object()) override;
    std::vector<EmbeddingResult> GenerateBatch(const std::vector<std::string>& inputs,
                                                const nlohmann::json& params = nlohmann::json::object()) override;
    int GetEmbeddingDimension() const override;
    nlohmann::json GetAvailableModels() const override;

    // Configuration
    bool SetModelPath(const std::string& path);
    std::string GetModelPath() const;

private:
    mutable std::mutex mutex_;
    std::string model_path_;
    std::string model_name_;
    BackendStatus status_{BackendStatus::UNINITIALIZED};
    int embedding_dim_{768};
    nlohmann::json available_models_ = nlohmann::json::array();
};

} // namespace inferdeck::backends
