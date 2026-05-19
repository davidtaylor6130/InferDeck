#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>

#include <llama.h>
#include "llama_cpp/VulkanDevice.hpp"
#include "llama_cpp/GGUFParser.hpp"

namespace inferdeck::core {

enum class MessageRole {
    System,
    User,
    Assistant,
    Tool
};

struct ChatMessage {
    MessageRole role;
    std::string content;
};

struct InferenceParams {
    int max_tokens = 256;
    float temperature = 0.7f;
    float top_p = 0.9f;
    float repetition_penalty = 1.0f;
    bool stream = false;
    std::string stop;
    std::vector<float> presence_penalty;
    std::string user;
};

struct InferenceResult {
    std::string text;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    float duration_ms = 0.0f;
};

struct InferenceStats {
    uint64_t total_requests = 0;
    uint64_t successful_requests = 0;
    uint64_t failed_requests = 0;
    float avg_latency_ms = 0.0f;
    float max_latency_ms = 0.0f;
    float min_latency_ms = 999999.0f;
    uint64_t tokens_generated = 0;
    uint64_t tokens_processed = 0;
};

using TokenCallback = std::function<void(const std::string& token, int cumulative_tokens)>;

class LlamaEngine {
public:
    bool Initialize(const std::string& model_path,
                    const std::string& precision = "auto",
                    int gpu_layers = -1,
                    int context_size = 4096);

    static LlamaEngine& Get();
    bool IsInitialized() const;
    InferenceResult Predict(const std::vector<ChatMessage>& messages,
                            const InferenceParams& params = {});
    InferenceResult PredictStream(const std::vector<ChatMessage>& messages,
                                   const InferenceParams& params = {},
                                   TokenCallback on_token = nullptr);
    InferenceStats GetStats() const;
    std::string GetModelName() const;
    std::string GetPrecision() const;
    auto GetGgufMetadata() const { return gguf_metadata_; }
    void Shutdown();
    GpuInfo GetGpuInfo() const;

private:
    LlamaEngine();
    ~LlamaEngine();
    LlamaEngine(const LlamaEngine&) = delete;
    LlamaEngine& operator=(const LlamaEngine&) = delete;

    std::string BuildPrompt(const std::vector<ChatMessage>& messages) const;
    llama_token SampleToken(const float* logits, int n_vocab, float temperature, float top_p);

    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    const llama_vocab* vocab_ = nullptr;

    std::string model_path_;
    std::string precision_;
    int gpu_layers_ = -1;
    int context_size_ = 4096;

    GGUFMetadata gguf_metadata_;
    std::vector<std::string> vocab_tokens_;
    InferenceStats stats_;
    mutable std::mutex mutex_;
    bool initialized_ = false;
};

} // namespace inferdeck::core
