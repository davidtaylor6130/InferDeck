/// @file test_image_backend.cpp
/// @brief Tests for image generation backend interface and mock implementation.

#include <catch2/catch_test_macros.hpp>
#include "backends/IImageBackend.hpp"
#include "backends/ITextBackend.hpp"
#include <memory>

namespace {

using namespace inferdeck::backends;

class MockImageBackend : public IImageBackend {
public:
    std::string GetBackendName() const override { return "mock_sd"; }
    BackendStatus GetStatus() const override { return status_; }
    bool Initialize() override { status_ = BackendStatus::READY; return true; }
    void Shutdown() override { status_ = BackendStatus::UNINITIALIZED; }
    nlohmann::json GetInfo() const override { return nlohmann::json{{"name", "mock_sd"}}; }
    nlohmann::json GetVRAMUsage() const override { return nlohmann::json{{"used", 4ULL << 30}}; }
    bool IsReady() const override { return status_ == BackendStatus::READY; }

    ImageGenerationResult GenerateTextToImage(const std::string& prompt,
                                                const nlohmann::json&) override {
        ImageGenerationResult result;
        result.output_path = "output/img_txt2img.png";
        result.width = 1024;
        result.height = 1024;
        result.num_steps = 30;
        result.inference_time_ms = 2500.0;
        result.model_name = "sdxl";
        result.img2img = false;
        result.output_data = std::vector<uint8_t>(100, 0xFF);
        return result;
    }

    ImageGenerationResult GenerateImageToImage(const std::string&, const std::string& prompt,
                                                const nlohmann::json&) override {
        ImageGenerationResult result;
        result.output_path = "output/img_img2img.png";
        result.width = 512;
        result.height = 512;
        result.num_steps = 20;
        result.inference_time_ms = 1800.0;
        result.model_name = "sdxl";
        result.img2img = true;
        result.output_data = std::vector<uint8_t>(50, 0xEE);
        return result;
    }

    nlohmann::json GetAvailableCheckpoints() const override {
        return nlohmann::json{{"sdxl", "Stable Diffusion XL"}, {"sdxl-turbo", "SDXL Turbo"}};
    }
    bool SetCheckpoint(const std::string& name) override {
        current_checkpoint_ = name;
        return true;
    }
    std::string GetCheckpointName() const override { return current_checkpoint_; }

    std::vector<ImageGenerationResult> BatchGenerate(
        const std::vector<nlohmann::json>& requests) override {
        std::vector<ImageGenerationResult> results;
        for (const auto& req : requests) {
            results.push_back(GenerateTextToImage(req.value("prompt", std::string{}), req));
        }
        return results;
    }

    BackendStatus status_ = BackendStatus::UNINITIALIZED;
    std::string current_checkpoint_ = "sdxl";
};

} // namespace

TEST_CASE("Image: Mock backend name and status", "[image][mock]") {
    MockImageBackend backend;
    REQUIRE(backend.GetBackendName() == "mock_sd");
    REQUIRE(!backend.IsReady());
}

TEST_CASE("Image: Initialize and shutdown", "[image][mock]") {
    MockImageBackend backend;
    REQUIRE(backend.Initialize());
    REQUIRE(backend.IsReady());
    backend.Shutdown();
    REQUIRE(!backend.IsReady());
}

TEST_CASE("Image: GenerateTextToImage returns valid result", "[image][mock]") {
    MockImageBackend backend;
    backend.Initialize();

    auto result = backend.GenerateTextToImage("a cat");
    REQUIRE(result.output_path.find("img2img") == std::string::npos);
    REQUIRE(result.width == 1024);
    REQUIRE(result.height == 1024);
    REQUIRE(result.num_steps == 30);
    REQUIRE(result.inference_time_ms == 2500.0);
    REQUIRE(result.model_name == "sdxl");
    REQUIRE(result.img2img == false);
    REQUIRE(result.output_data.size() == 100);
}

TEST_CASE("Image: GenerateImageToImage returns valid result", "[image][mock]") {
    MockImageBackend backend;
    backend.Initialize();

    auto result = backend.GenerateImageToImage("input.png", "modify image", {});
    REQUIRE(result.output_path.find("img2img") != std::string::npos);
    REQUIRE(result.width == 512);
    REQUIRE(result.height == 512);
    REQUIRE(result.num_steps == 20);
    REQUIRE(result.img2img == true);
    REQUIRE(result.output_data.size() == 50);
}

TEST_CASE("Image: GetAvailableCheckpoints returns object", "[image][mock]") {
    MockImageBackend backend;
    auto checkpoints = backend.GetAvailableCheckpoints();
    REQUIRE(checkpoints.is_object());
    REQUIRE(checkpoints.contains("sdxl"));
    REQUIRE(checkpoints.contains("sdxl-turbo"));
}

TEST_CASE("Image: SetCheckpoint changes checkpoint", "[image][mock]") {
    MockImageBackend backend;
    REQUIRE(backend.SetCheckpoint("sdxl-turbo"));
    REQUIRE(backend.GetCheckpointName() == "sdxl-turbo");
}

TEST_CASE("Image: Default checkpoint is sdxl", "[image][mock]") {
    MockImageBackend backend;
    REQUIRE(backend.GetCheckpointName() == "sdxl");
}

TEST_CASE("Image: BatchGenerate processes multiple requests", "[image][mock]") {
    MockImageBackend backend;
    backend.Initialize();

    std::vector<nlohmann::json> requests;
    requests.push_back({{"prompt", "first image"}});
    requests.push_back({{"prompt", "second image"}});

    auto results = backend.BatchGenerate(requests);
    REQUIRE(results.size() == 2);
    REQUIRE(results[0].output_path.find("img2img") == std::string::npos);
    REQUIRE(results[1].output_path.find("img2img") == std::string::npos);
}

TEST_CASE("Image: GetVRAMUsage returns bytes", "[image][mock]") {
    MockImageBackend backend;
    auto vram = backend.GetVRAMUsage();
    REQUIRE(vram.is_object());
    REQUIRE(vram.contains("used"));
    REQUIRE(vram["used"].get<uint64_t>() == (4ULL << 30)); // 4GB
}

TEST_CASE("Image: Cast to ITextBackend interface", "[image][mock]") {
    std::unique_ptr<ITextBackend> base = std::make_unique<MockImageBackend>();
    REQUIRE(base->Initialize());
    REQUIRE(base->IsReady());
    base->Shutdown();
}

TEST_CASE("Image: Cast to IImageBackend interface", "[image][mock]") {
    std::unique_ptr<IImageBackend> img = std::make_unique<MockImageBackend>();
    REQUIRE(img->GetBackendName() == "mock_sd");
    auto result = img->GenerateTextToImage("test", {});
    REQUIRE(result.width == 1024);
    REQUIRE(result.height == 1024);
}
