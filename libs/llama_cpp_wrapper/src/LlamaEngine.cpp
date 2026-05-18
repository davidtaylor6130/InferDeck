/// @file LlamaEngine.cpp
/// @brief LlamaEngine implementation with real llama.cpp integration.

#include "llama_cpp/LlamaEngine.hpp"
#include "llama_cpp/GGUFParser.hpp"
#include "llama_cpp/VulkanDevice.hpp"
#include "core/Logger.hpp"
#include "core/Metrics.hpp"

#include <llama.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <random>
#include <thread>

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

    // Validate model file exists
    if (!std::filesystem::exists(model_path)) {
        Logger::Get().Error("Model file not found: " + model_path);
        return false;
    }

    // Parse GGUF metadata for validation
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

    Logger::Get().Info("Loading model: " + model_path);
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

    // Configure llama.cpp model parameters
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = gpu_layers;
    model_params.main_gpu = 0;

    // Load the model
    model_ = llama_load_model_from_file(model_path.c_str(), model_params);
    if (!model_) {
        Logger::Get().Error("Failed to load model: " + model_path);
        return false;
    }

    Logger::Get().Info("Model loaded successfully: " + metadata.model_name);
    Logger::Get().Info("Architecture: " + GGUFParser::ArchToString(metadata.architecture));
    Logger::Get().Info("Vocab size: " + std::to_string(metadata.vocab_size));
    Logger::Get().Info("Block count: " + std::to_string(metadata.block_count));

    // Configure context parameters
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = context_size;
    ctx_params.n_batch = metadata.vocab_size;
    ctx_params.n_ubatch = 512;
    ctx_params.n_seq_max = 1;
    ctx_params.n_threads = std::thread::hardware_concurrency();
    ctx_params.n_threads_batch = ctx_params.n_threads;
    ctx_params.embeddings = true;
    ctx_params.flash_attn = true;
    ctx_params.no_kv_offload = false;
    ctx_params.compute_type = GG_TYPE_F16;
    ctx_params.causal_attn = true;

    // Create context
    ctx_ = llama_new_context_with_model(model_, ctx_params);
    if (!ctx_) {
        Logger::Get().Error("Failed to create context");
        llama_free_model(model_);
        model_ = nullptr;
        return false;
    }

    Logger::Get().Info("Context created with " + std::to_string(context_size) + " tokens");
    Logger::Get().Info("Batch size: " + std::to_string(ctx_params.n_batch));

    // Initialize tokenizer
    vocab_tokens_.resize(llama_vocab_size(model_));
    for (int i = 0; i < llama_vocab_size(model_); i++) {
        vocab_tokens_[i] = llama_token_to_piece(ctx_, i);
    }

    initialized_ = true;
    return true;
}

bool LlamaEngine::IsInitialized() const {
    return initialized_;
}

InferenceResult LlamaEngine::Predict(const std::vector<ChatMessage>& messages,
                                      const InferenceParams& params) {
    auto start = std::chrono::high_resolution_clock::now();

    // Build prompt from messages
    std::string prompt = BuildPrompt(messages);

    // Tokenize
    int n_ctx = llama_n_ctx(ctx_);
    std::vector<llama_token> tokens(n_ctx, 0);
    int n_tokens = llama_tokenize(ctx_, prompt.c_str(), prompt.size(), tokens.data(), n_ctx, true, false);
    if (n_tokens < 0) {
        n_tokens = -n_tokens;
        tokens.resize(n_tokens);
        llama_tokenize(ctx_, prompt.c_str(), prompt.size(), tokens.data(), n_tokens, true, false);
    }

    Logger::Get().Info("Prompt tokenized: " + std::to_string(n_tokens) + " tokens");

    // Run inference
    std::vector<llama_token> output_tokens;
    output_tokens.reserve(params.max_tokens);

    // Use first token to start generation
    llama_token prev_token = tokens[n_tokens - 1];

    for (int i = 0; i < params.max_tokens; i++) {
        // Get logits from last position
        const llama_logits* logits = llama_get_logits_item(ctx_, -1);
        if (!logits) {
            Logger::Get().Error("Failed to get logits");
            break;
        }

        // Sample next token using nucleus sampling
        llama_token next_token = SampleToken(logits, params.temperature, params.top_p);

        // Check for EOS token
        if (next_token == llama_token_eos(ctx_) || next_token == llama_token_eot(ctx_)) {
            break;
        }

        output_tokens.push_back(next_token);

        // Decode the token to text
        std::string token_str = llama_token_to_piece(ctx_, next_token);

        // Call streaming callback if provided
        if (params.stream) {
            // We'll handle this in PredictStream instead
        }

        // Prepare for next iteration
        std::vector<llama_token> batch = {next_token};
        if (llama_decode(ctx_, llama_batch_get_one(batch.data(), batch.size())) != 0) {
            Logger::Get().Error("Failed to decode next token");
            break;
        }

        prev_token = next_token;
    }

    // Detokenize output
    std::string result;
    for (auto token : output_tokens) {
        std::string piece = llama_token_to_piece(ctx_, token);
        result += piece;
    }

    auto end = std::chrono::high_resolution_clock::now();
    float duration = std::chrono::duration<float, std::milli>(end - start).count();

    // Update stats
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.total_requests++;
        stats_.successful_requests++;
        stats_.avg_latency_ms = (stats_.avg_latency_ms * (stats_.successful_requests - 1) + duration) / stats_.successful_requests;
        stats_.max_latency_ms = std::max(stats_.max_latency_ms, duration);
        stats_.min_latency_ms = std::min(stats_.min_latency_ms, duration);
        stats_.tokens_generated += output_tokens.size();
        stats_.tokens_processed += n_tokens;
    }

    // Record metrics
    MetricsStore::Get().IncrementCounter("inferdeck.requests.total", 1);
    MetricsStore::Get().IncrementCounter("inferdeck.requests.success", 1);
    MetricsStore::Get().RecordHistogram("inferdeck.latency_ms", duration);
    MetricsStore::Get().RecordHistogram("inferdeck.prompt_tokens", n_tokens);
    MetricsStore::Get().RecordHistogram("inferdeck.completion_tokens", output_tokens.size());

    Logger::Get().Info("Inference complete: " + std::to_string(output_tokens.size()) + " tokens in " +
                       std::to_string(duration) + "ms");

    return InferenceResult{
        .text = result,
        .prompt_tokens = n_tokens,
        .completion_tokens = output_tokens.size(),
        .total_tokens = n_tokens + output_tokens.size(),
        .duration_ms = duration
    };
}

InferenceResult LlamaEngine::PredictStream(const std::vector<ChatMessage>& messages,
                                             const InferenceParams& params,
                                             TokenCallback on_token) {
    auto start = std::chrono::high_resolution_clock::now();

    // Build prompt from messages
    std::string prompt = BuildPrompt(messages);

    // Tokenize
    int n_ctx = llama_n_ctx(ctx_);
    std::vector<llama_token> tokens(n_ctx, 0);
    int n_tokens = llama_tokenize(ctx_, prompt.c_str(), prompt.size(), tokens.data(), n_ctx, true, false);
    if (n_tokens < 0) {
        n_tokens = -n_tokens;
        tokens.resize(n_tokens);
        llama_tokenize(ctx_, prompt.c_str(), prompt.size(), tokens.data(), n_tokens, true, false);
    }

    Logger::Get().Info("Prompt tokenized: " + std::to_string(n_tokens) + " tokens (streaming)");

    // Run inference with token-by-token streaming
    std::vector<llama_token> output_tokens;
    output_tokens.reserve(params.max_tokens);

    // Use first token to start generation
    llama_token prev_token = tokens[n_tokens - 1];
    int cumulative = 0;

    for (int i = 0; i < params.max_tokens; i++) {
        // Get logits from last position
        const llama_logits* logits = llama_get_logits_item(ctx_, -1);
        if (!logits) {
            Logger::Get().Error("Failed to get logits");
            break;
        }

        // Sample next token using nucleus sampling
        llama_token next_token = SampleToken(logits, params.temperature, params.top_p);

        // Check for EOS token
        if (next_token == llama_token_eos(ctx_) || next_token == llama_token_eot(ctx_)) {
            break;
        }

        output_tokens.push_back(next_token);

        // Decode the token to text
        std::string token_str = llama_token_to_piece(ctx_, next_token);

        // Call streaming callback
        if (on_token) {
            on_token(token_str, cumulative);
            cumulative++;
        }

        // Prepare for next iteration
        std::vector<llama_token> batch = {next_token};
        if (llama_decode(ctx_, llama_batch_get_one(batch.data(), batch.size())) != 0) {
            Logger::Get().Error("Failed to decode next token");
            break;
        }

        prev_token = next_token;
    }

    // Detokenize output for final result
    std::string result;
    for (auto token : output_tokens) {
        std::string piece = llama_token_to_piece(ctx_, token);
        result += piece;
    }

    auto end = std::chrono::high_resolution_clock::now();
    float duration = std::chrono::duration<float, std::milli>(end - start).count();

    // Update stats
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.total_requests++;
        stats_.successful_requests++;
        stats_.avg_latency_ms = (stats_.avg_latency_ms * (stats_.successful_requests - 1) + duration) / stats_.successful_requests;
        stats_.max_latency_ms = std::max(stats_.max_latency_ms, duration);
        stats_.min_latency_ms = std::min(stats_.min_latency_ms, duration);
        stats_.tokens_generated += output_tokens.size();
        stats_.tokens_processed += n_tokens;
    }

    // Record metrics
    MetricsStore::Get().IncrementCounter("inferdeck.requests.total", 1);
    MetricsStore::Get().IncrementCounter("inferdeck.requests.success", 1);
    MetricsStore::Get().RecordHistogram("inferdeck.latency_ms", duration);
    MetricsStore::Get().RecordHistogram("inferdeck.prompt_tokens", n_tokens);
    MetricsStore::Get().RecordHistogram("inferdeck.completion_tokens", output_tokens.size());

    Logger::Get().Info("Streaming inference complete: " + std::to_string(output_tokens.size()) +
                       " tokens in " + std::to_string(duration) + "ms");

    return InferenceResult{
        .text = result,
        .prompt_tokens = n_tokens,
        .completion_tokens = output_tokens.size(),
        .total_tokens = n_tokens + output_tokens.size(),
        .duration_ms = duration
    };
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
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    if (model_) {
        llama_free_model(model_);
        model_ = nullptr;
    }
    initialized_ = false;
    Logger::Get().Info("LlamaEngine shut down");
}

GpuInfo LlamaEngine::GetGpuInfo() const {
    return VulkanDevice::Get().GetBestGpu();
}

std::string LlamaEngine::BuildPrompt(const std::vector<ChatMessage>& messages) const {
    // Use llama.cpp chat template if available
    std::string prompt;

    // For V1, use a simple chat format
    // In production, this would use: llama_chat_apply_template()
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

llama_token LlamaEngine::SampleToken(const llama_logits* logits, float temperature, float top_p) {
    // Get vocabulary size
    int n_vocab = llama_n_vocab(model_);

    // Convert logits to probabilities with temperature
    std::vector<float> probs(n_vocab);
    float sum_exp = 0.0f;

    for (int i = 0; i < n_vocab; i++) {
        float logit = logits[i];
        if (temperature > 0.0f) {
            logit /= temperature;
        }
        probs[i] = std::exp(logit - *std::max_element(logits, logits + n_vocab));
        sum_exp += probs[i];
    }

    // Normalize probabilities
    for (int i = 0; i < n_vocab; i++) {
        probs[i] /= sum_exp;
    }

    // Apply top-p (nucleus) sampling
    std::vector<std::pair<float, int>> sorted_probs;
    sorted_probs.reserve(n_vocab);
    for (int i = 0; i < n_vocab; i++) {
        sorted_probs.push_back({probs[i], i});
    }
    std::sort(sorted_probs.begin(), sorted_probs.end(), std::greater<>());

    // Find top-p cutoff
    float cumulative = 0.0f;
    int cutoff_idx = n_vocab;
    for (int i = 0; i < n_vocab; i++) {
        cumulative += sorted_probs[i].first;
        if (cumulative >= top_p) {
            cutoff_idx = i + 1;
            break;
        }
    }

    // Renormalize probabilities for top-p
    float renorm_sum = 0.0f;
    for (int i = 0; i < cutoff_idx; i++) {
        renorm_sum += sorted_probs[i].first;
    }
    for (int i = 0; i < cutoff_idx; i++) {
        sorted_probs[i].first /= renorm_sum;
    }

    // Sample from top-p distribution
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);

    float r = dis(gen);
    cumulative = 0.0f;
    for (int i = 0; i < cutoff_idx; i++) {
        cumulative += sorted_probs[i].first;
        if (r <= cumulative) {
            return sorted_probs[i].second;
        }
    }

    // Fallback to last token
    return sorted_probs[cutoff_idx - 1].second;
}

} // namespace inferdeck::core
