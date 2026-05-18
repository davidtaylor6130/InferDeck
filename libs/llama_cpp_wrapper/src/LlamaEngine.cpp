/// @file LlamaEngine.cpp
/// @brief LlamaEngine implementation.

#include "llama_cpp/LlamaEngine.hpp"
#include "llama_cpp/GGUFParser.hpp"
#include "llama_cpp/VulkanDevice.hpp"
#include "core/Logger.hpp"
#include "core/Metrics.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>

namespace inferdeck::core {

LlamaEngine& LlamaEngine::Get() {
    static LlamaEngine instance;
    return instance;
}

LlamaEngine::LlamaEngine() = default;
LlamaEngine::~LlamaEngine() {
    Shutdown();
}

bool LlamaEngine::Initialize(const std::string& model_path,
                              const std::string& precision,
                              int gpu_layers,
                              int context_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    model_path_ = model_path;
    precision_ = precision;
    gpu_layers_ = gpu_layers;
    context_size_ = context_size;

    // Validate model file
    GGUFMetadata metadata = GGUFParser::Parse(std::filesystem::path(model_path));
    if (!metadata.valid) {
        Logger::Get().Error("Failed to parse GGUF model: " + model_path);
        return false;
    }

    gguf_metadata_ = metadata;

    // Auto-detect precision from GGUF if requested
    std::string actual_precision = precision;
    if (precision == "auto") {
        actual_precision = GGUFParser::QuantToString(metadata.quantization);
    }

    Logger::Get().Info("Initializing LlamaEngine with model: " + model_path);
    Logger::Get().Info("Precision: " + actual_precision);
    Logger::Get().Info("GPU layers: " + std::to_string(gpu_layers));
    Logger::Get().Info("Context size: " + std::to_string(context_size));

    // Get GPU info
    GpuInfo gpu = VulkanDevice::Get().GetBestGpu();
    if (gpu.name.empty()) {
        Logger::Get().Warn("No Vulkan GPU detected, falling back to CPU");
    } else {
        Logger::Get().Info("GPU: " + gpu.name);
        Logger::Get().Info("VRAM: " + std::to_string(gpu.memory_total / (1024 * 1024)) + " MB");
    }

    // TODO: Actually initialize llama.cpp model and context here
    // For V1, we use a placeholder initialization that validates the config
    // In production, this will call:
    //   model_ = llama_load_model_from_file(model_path.c_str(), params)
    //   ctx_ = llama_new_context_with_model(model_, ctx_params)

    initialized_ = true;
    return true;
}

bool LlamaEngine::IsInitialized() const {
    return initialized_;
}

InferenceResult LlamaEngine::Predict(const std::vector<ChatMessage>& messages,
                                      const InferenceParams& params) {
    auto start = std::chrono::high_resolution_clock::now();

    // Build prompt
    std::string prompt = BuildPrompt(messages);

    // Tokenize
    std::vector<int> tokens = Tokenize(prompt);

    // Run inference (placeholder for V1)
    // TODO: Replace with actual llama.cpp inference
    std::vector<int> output_tokens;

    // Detokenize
    std::string result = Detokenize(output_tokens);

    auto end = std::chrono::high_resolution_clock::now();
    float duration = std::chrono::duration<float, std::milli>(end - start).count();

    // Update stats
    stats_.total_requests++;
    stats_.successful_requests++;
    stats_.avg_latency_ms = (stats_.avg_latency_ms * (stats_.successful_requests - 1) + duration) / stats_.successful_requests;
    stats_.max_latency_ms = std::max(stats_.max_latency_ms, duration);
    stats_.min_latency_ms = std::min(stats_.min_latency_ms, duration);

    // Record histogram
    MetricsStore::Get().IncrementCounter("inferdeck.requests.total", 1);
    MetricsStore::Get().IncrementCounter("inferdeck.requests.success", 1);
    MetricsStore::Get().RecordHistogram("inferdeck.latency_ms", duration);

    return InferenceResult{
        .text = result,
        .prompt_tokens = static_cast<int>(tokens.size()),
        .completion_tokens = static_cast<int>(output_tokens.size()),
        .total_tokens = static_cast<int>(tokens.size() + output_tokens.size()),
        .duration_ms = duration
    };
}

InferenceResult LlamaEngine::PredictStream(const std::vector<ChatMessage>& messages,
                                            const InferenceParams& params,
                                            TokenCallback on_token) {
    // For V1, streaming uses the same logic as non-streaming
    // TODO: Implement true streaming with llama.cpp kv cache access
    InferenceResult result = Predict(messages, params);

    if (on_token && !result.text.empty()) {
        int cumulative = 0;
        for (char c : result.text) {
            on_token(std::string(1, c), cumulative);
            cumulative++;
        }
    }

    return result;
}

InferenceStats LlamaEngine::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::string LlamaEngine::GetModelName() const {
    return gguf_metadata_.model_name;
}

std::string LlamaEngine::GetPrecision() const {
    return precision_;
}

void LlamaEngine::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ctx_) {
        // llama_free(ctx_);
        ctx_ = nullptr;
    }
    if (model_) {
        // llama_free_model(model_);
        model_ = nullptr;
    }
    initialized_ = false;
}

GpuInfo LlamaEngine::GetGpuInfo() const {
    return VulkanDevice::Get().GetBestGpu();
}

std::string LlamaEngine::BuildPrompt(const std::vector<ChatMessage>& messages) const {
    // Simple chat format for V1
    // TODO: Replace with actual model-specific chat template
    std::string prompt;
    for (const auto& msg : messages) {
        std::string role_str;
        switch (msg.role) {
            case MessageRole::System:  role_str = "system"; break;
            case MessageRole::User:    role_str = "user"; break;
            case MessageRole::Assistant: role_str = "assistant"; break;
            case MessageRole::Tool:    role_str = "tool"; break;
        }
        prompt += "<|" + role_str + "|>" + msg.content + "<|end|>\n";
    }
    prompt += "<|assistant|>";
    return prompt;
}

std::vector<int> LlamaEngine::Tokenize(const std::string& text) const {
    // TODO: Replace with actual llama_tokenize
    // For V1, return empty vector as placeholder
    return {};
}

std::string LlamaEngine::Detokenize(const std::vector<int>& tokens) const {
    // TODO: Replace with actual llama_detokenize
    return "";
}

bool LlamaEngine::RunInference(const std::vector<int>& input_tokens,
                                std::vector<int>& output_tokens,
                                const InferenceParams& params) {
    // TODO: Implement actual llama.cpp inference
    return true;
}

} // namespace inferdeck::core
