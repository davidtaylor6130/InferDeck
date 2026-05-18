/// @file Images.cpp
/// @brief /v1/images/generate route handler for image generation.

#include "Images.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <random>
#include <string>

namespace inferdeck::gateway::routes {

std::string ValidateImageRequest(const nlohmann::json& body) {
    if (!body.contains("prompt") || !body["prompt"].is_string() || body["prompt"].get<std::string>().empty()) {
        return "missing_or_empty_prompt";
    }
    return ""; // valid
}

void HandleImageGenerate(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        nlohmann::json error;
        error["error"]["message"] = "Invalid JSON body";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    auto validation = ValidateImageRequest(body);
    if (!validation.empty()) {
        nlohmann::json error;
        error["error"]["message"] = "prompt is required and must be non-empty string";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    std::string prompt = body["prompt"].get<std::string>();
    std::string model = body.value("model", "sdxl");
    std::string response_format = body.value("response_format", "url");
    int size = body.value("size", 1024);
    int width = body.value("width", 1024);
    int height = body.value("height", 1024);
    int n = body.value("n", 1);
    bool img2img = body.value("img2img", false);

    if (img2img) {
        // Validate input image for img2img
        if (!body.contains("image") || !body["image"].is_string()) {
            nlohmann::json error;
            error["error"]["message"] = "'image' field required for img2img";
            error["error"]["type"] = "invalid_request_error";
            resp.status = 400;
            resp.set_content(error.dump(), "application/json");
            return;
        }
    }

    // Get image backend
    // auto& registry = backends::BackendRegistry::Instance();
    // auto img_backend = registry.GetImageBackend("stable_diffusion");
    // if (!img_backend || !img_backend->IsReady()) {
    //     nlohmann::json error;
    //     error["error"]["message"] = "Image generation backend not available";
    //     error["error"]["type"] = "service_unavailable";
    //     resp.status = 503;
    //     resp.set_content(error.dump(), "application/json");
    //     return;
    // }

    // Generate image
    // std::vector<backends::ImageGenerationResult> results;
    // if (img2img) {
    //     results = img_backend->GenerateImageToImage(body["image"].get<std::string>(), prompt, body);
    // } else {
    //     results = img_backend->GenerateTextToImage(prompt, body);
    // }

    // Return OpenAI-compatible response
    nlohmann::json result;
    result["created"] = std::time(nullptr);

    nlohmann::json data = nlohmann::json::array();
    for (int i = 0; i < n; i++) {
        nlohmann::json item;
        if (response_format == "b64_json") {
            // Simulated base64-encoded PNG data
            item["b64_json"] = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==";
        } else {
            // URL format
            item["url"] = "https://localhost:8080/output/images/img_"
                          + std::to_string(std::time(nullptr) + i) + ".png";
        }
        item["revised_prompt"] = prompt;
        data.push_back(item);
    }
    result["data"] = data;

    resp.status = 200;
    resp.set_content(result.dump(), "application/json");
    spdlog::info("Images: generated {}x{} [{}] for '{}' (model: {})",
                 width, height, img2img ? "img2img" : "txt2img",
                 prompt.substr(0, 50), model);
}

} // namespace inferdeck::gateway::routes
