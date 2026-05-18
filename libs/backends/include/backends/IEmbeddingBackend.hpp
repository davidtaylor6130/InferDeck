/// @file IEmbeddingBackend.hpp
/// @brief Embedding generation backend interface.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>
#include "backends/ITextBackend.hpp"

namespace inferdeck::backends {

struct EmbeddingResult {
    std::vector<float> embedding;
    int token_count;
    double inference_time_ms;
    std::string model_name;
    nlohmann::json metadata;
};

class IEmbeddingBackend : public ITextBackend {
public:
    ~IEmbeddingBackend() override = default;

    virtual EmbeddingResult Generate(const std::string& input,
                                      const nlohmann::json& params = nlohmann::json::object()) = 0;

    virtual std::vector<EmbeddingResult> GenerateBatch(const std::vector<std::string>& inputs,
                                                        const nlohmann::json& params = nlohmann::json::object()) = 0;

    virtual int GetEmbeddingDimension() const = 0;
    virtual nlohmann::json GetAvailableModels() const = 0;
};

} // namespace inferdeck::backends
