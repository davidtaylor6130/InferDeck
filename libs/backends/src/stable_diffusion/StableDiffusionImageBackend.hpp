/// @file StableDiffusionImageBackend.hpp
/// @brief stable-diffusion.cpp GGML-based image generation backend.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "backends/IImageBackend.hpp"

namespace inferdeck::backends {

struct StableDiffusionConfig {
    std::string checkpoint_path;
    std::string checkpoint_name;
    std::string prompt;
    std::string negative_prompt;
    int width = 512;
    int height = 512;
    int steps = 20;
    float cfg_scale = 7.0f;
    float seed = 42.0f;
    std::string output_format = "png";
    int num_images = 1;
};

class StableDiffusionImageBackend : public IImageBackend {
public:
    StableDiffusionImageBackend() = default;
    ~StableDiffusionImageBackend() override = default;

    std::string GetBackendName() const override;
    BackendStatus GetStatus() const override;
    bool Initialize() override;
    void Shutdown() override;
    nlohmann::json GetInfo() const override;
    nlohmann::json GetVRAMUsage() const override;
    bool IsReady() const override;

    ImageGenerationResult GenerateTextToImage(const std::string& prompt,
                                                const nlohmann::json& params) override;
    ImageGenerationResult GenerateImageToImage(const std::string& input_image_path,
                                                const std::string& prompt,
                                                const nlohmann::json& params) override;
    nlohmann::json GetAvailableCheckpoints() const override;
    bool SetCheckpoint(const std::string& checkpoint_name) override;
    std::string GetCheckpointName() const override;
    std::vector<ImageGenerationResult> BatchGenerate(
        const std::vector<nlohmann::json>& requests) override;

    // Configuration
    bool SetCheckpointPath(const std::string& path);
    std::string GetCheckpointPath() const;
    void SetVRAMThreshold(uint64_t threshold);
    uint64_t GetVRAMThreshold() const;

private:
    mutable std::mutex mutex_;
    std::string checkpoint_path_;
    std::string checkpoint_name_;
    BackendStatus status_{BackendStatus::UNINITIALIZED};
    uint64_t vram_threshold_{4ULL * 1024 * 1024 * 1024}; // 4GB
    std::unordered_map<std::string, std::string> checkpoints_;
    int current_batch_size_{1};
};

} // namespace inferdeck::backends
