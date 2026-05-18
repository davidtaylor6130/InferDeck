/// @file IImageBackend.hpp
/// @brief Image generation backend interface.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>
#include "backends/ITextBackend.hpp"

namespace inferdeck::backends {

struct ImageGenerationResult {
    std::string output_path;
    std::vector<uint8_t> output_data;
    int width;
    int height;
    int num_steps;
    double inference_time_ms;
    std::string model_name;
    bool img2img;
    nlohmann::json metadata;
};

class IImageBackend : public ITextBackend {
public:
    ~IImageBackend() override = default;

    virtual ImageGenerationResult GenerateTextToImage(const std::string& prompt,
                                                      const nlohmann::json& params = nlohmann::json::object()) = 0;

    virtual ImageGenerationResult GenerateImageToImage(const std::string& input_image_path,
                                                       const std::string& prompt,
                                                       const nlohmann::json& params = nlohmann::json::object()) = 0;

    virtual nlohmann::json GetAvailableCheckpoints() const = 0;
    virtual bool SetCheckpoint(const std::string& checkpoint_name) = 0;
    virtual std::string GetCheckpointName() const = 0;
    virtual std::vector<ImageGenerationResult> BatchGenerate(
        const std::vector<nlohmann::json>& requests) = 0;
};

} // namespace inferdeck::backends
