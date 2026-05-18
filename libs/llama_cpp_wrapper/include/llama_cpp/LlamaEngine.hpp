/// @file LlamaEngine.hpp
/// @brief LlamaEngine — High-level wrapper for llama.cpp inference.
///
/// Provides the main inference interface for InferDeck:
/// - Model loading with auto-detect precision
/// - Non-streaming predictions (complete response)
/// - Streaming predictions (SSE-compatible token output)
/// - Mixed precision support (FP32, FP16, Q4, Q8)
/// - GPU acceleration via Vulkan
/// - Nucleus (top-p) sampling for text generation

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>

// Real llama.h integration
#include <llama.h>

namespace inferdeck::core {

/// Message role for chat completions.
enum class MessageRole {
    System,
    User,
    Assistant,
    Tool
};

/// Chat message with role and content.
struct ChatMessage {
    MessageRole role;
    std::string content;
};

/// Inference request parameters.
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

/// Inference result (non-streaming).
struct InferenceResult {
    std::string text;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    float duration_ms = 0.0f;
};

/// Inference stats for monitoring.
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

/// Token callback type for streaming.
/// @param token The token text to stream.
/// @param cumulative_tokens Total tokens processed so far.
using TokenCallback = std::function<void(const std::string& token, int cumulative_tokens)>;

/// LlamaEngine manages model loading, inference, and GPU resources.
///
/// This is the core inference class that wraps llama.cpp functionality.
/// It handles model loading with auto-detect precision, tokenization,
/// inference execution, and GPU memory management.
class LlamaEngine {
public:
    /// Load and initialize the LlamaEngine.
    /// @param model_path Path to the GGUF model file.
    /// @param precision Precision mode: "auto", "f32", "f16", "q4_0", "q4_k", "q8_0".
    /// @param gpu_layers Number of layers to offload to GPU (-1 = all).
    /// @param context_size Size of the context window.
    /// @return True if engine initialized successfully.
    bool Initialize(const std::string& model_path,
                    const std::string& precision = "auto",
                    int gpu_layers = -1,
                    int context_size = 4096);

    /// Get the singleton LlamaEngine instance.
    /// @return Reference to the singleton LlamaEngine instance.
    static LlamaEngine& Get();

    /// Check if the engine is initialized.
    /// @return True if the engine is ready for inference.
    bool IsInitialized() const;

    /// Run a non-streaming prediction.
    /// @param messages Chat messages to generate from.
    /// @param params Inference parameters.
    /// @return The inference result.
    InferenceResult Predict(const std::vector<ChatMessage>& messages,
                            const InferenceParams& params = {});

    /// Run a streaming prediction.
    /// @param messages Chat messages to generate from.
    /// @param params Inference parameters.
    /// @param on_token Callback invoked for each generated token.
    /// @return The final inference result.
    InferenceResult PredictStream(const std::vector<ChatMessage>& messages,
                                   const InferenceParams& params = {},
                                   TokenCallback on_token = nullptr);

    /// Get inference statistics.
    /// @return Current inference stats.
    InferenceStats GetStats() const;

    /// Get the model name.
    /// @return The model name, or empty string.
    std::string GetModelName() const;

    /// Get the current precision.
    /// @return The precision string.
    std::string GetPrecision() const;

    /// Get the GGUF metadata.
    /// @return The parsed GGUF metadata.
    auto GetGgufMetadata() const { return gguf_metadata_; }

    /// Shutdown the engine and release resources.
    void Shutdown();

    /// Get GPU information.
    /// @return Current GPU info.
    GpuInfo GetGpuInfo() const;

private:
    LlamaEngine();
    ~LlamaEngine();
    LlamaEngine(const LlamaEngine&) = delete;
    LlamaEngine& operator=(const LlamaEngine&) = delete;

    /// Build the chat prompt from messages.
    std::string BuildPrompt(const std::vector<ChatMessage>& messages) const;

    /// Sample the next token from logits using nucleus sampling.
    /// @param logits The logits from the model.
    /// @param temperature Sampling temperature.
    /// @param top_p Top-p probability threshold.
    /// @return The sampled token ID.
    llama_token SampleToken(const llama_logits* logits, float temperature, float top_p);

    // Real llama.cpp handles
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;

    // Configuration
    std::string model_path_;
    std::string precision_;
    int gpu_layers_ = -1;
    int context_size_ = 4096;

    // Metadata
    GGUFMetadata gguf_metadata_;

    // Vocabulary cache (pre-computed token pieces)
    std::vector<std::string> vocab_tokens_;

    // Stats
    InferenceStats stats_;

    // Thread safety
    mutable std::mutex mutex_;
    bool initialized_ = false;
};

} // namespace inferdeck::core
