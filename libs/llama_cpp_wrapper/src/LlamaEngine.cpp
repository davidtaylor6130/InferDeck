#include "llama_cpp/LlamaEngine.hpp"
#include "llama_cpp/GGUFParser.hpp"
#include "llama_cpp/VulkanDevice.hpp"
#include "core/Logger.hpp"
#include "core/Metrics.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

namespace inferdeck::core {

static std::string token_to_piece(const llama_vocab* vocab, llama_token token) {
    char buf[32];
    int n = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, false);
    if (n < 0) {
        std::vector<char> big_buf(-n);
        llama_token_to_piece(vocab, token, big_buf.data(), -n, 0, false);
        return std::string(big_buf.data(), -n);
    }
    return std::string(buf, n);
}

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

    if (!std::filesystem::exists(model_path)) {
        Logger::Get().Error("Model file not found: " + model_path);
        return false;
    }

    GGUFMetadata metadata = GGUFParser::Parse(std::filesystem::path(model_path));
    if (!metadata.valid) {
        Logger::Get().Warn("Failed to parse GGUF model (continuing anyway): " + model_path);
    }

    gguf_metadata_ = metadata;

    std::string actual_precision = precision;
    if (precision == "auto" && metadata.valid) {
        actual_precision = GGUFParser::QuantToString(metadata.quantization);
    }

    Logger::Get().Info("Loading model: " + model_path);
    Logger::Get().Info("Precision: " + actual_precision);
    Logger::Get().Info("GPU layers: " + std::to_string(gpu_layers));
    Logger::Get().Info("Context size: " + std::to_string(context_size));

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = gpu_layers;

    model_ = llama_model_load_from_file(model_path.c_str(), model_params);
    if (!model_) {
        Logger::Get().Error("Failed to load model: " + model_path);
        return false;
    }

    vocab_ = llama_model_get_vocab(model_);

    Logger::Get().Info("Model loaded successfully");

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = context_size;
    ctx_params.n_batch = 512;
    ctx_params.n_ubatch = 512;
    ctx_params.n_seq_max = 1;
    ctx_params.n_threads = std::thread::hardware_concurrency();
    ctx_params.n_threads_batch = ctx_params.n_threads;
    ctx_params.embeddings = true;

    ctx_ = llama_init_from_model(model_, ctx_params);
    if (!ctx_) {
        Logger::Get().Error("Failed to create context");
        llama_free_model(model_);
        model_ = nullptr;
        return false;
    }

    Logger::Get().Info("Context created with " + std::to_string(context_size) + " tokens");

    int n_vocab = llama_vocab_n_tokens(vocab_);
    vocab_tokens_.resize(n_vocab);
    for (int i = 0; i < n_vocab; i++) {
        vocab_tokens_[i] = token_to_piece(vocab_, i);
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

    llama_memory_clear(llama_get_memory(ctx_), true);

    std::string prompt = BuildPrompt(messages);

    int n_ctx = llama_n_ctx(ctx_);
    std::vector<llama_token> tokens(n_ctx);
    int n_tokens = llama_tokenize(vocab_, prompt.c_str(), static_cast<int>(prompt.size()), tokens.data(), n_ctx, true, false);
    if (n_tokens < 0) {
        n_tokens = -n_tokens;
        tokens.resize(n_tokens);
        llama_tokenize(vocab_, prompt.c_str(), static_cast<int>(prompt.size()), tokens.data(), n_tokens, true, false);
    }

    Logger::Get().Info("Prompt tokenized: " + std::to_string(n_tokens) + " tokens");

    std::vector<llama_token> output_tokens;
    output_tokens.reserve(params.max_tokens);

    llama_batch batch = llama_batch_init(512, 0, 1);

    for (int i = 0; i < n_tokens && i < 512; i++) {
        batch.token[batch.n_tokens] = tokens[i];
        batch.pos[batch.n_tokens] = i;
        batch.n_seq_id[batch.n_tokens] = 1;
        batch.seq_id[batch.n_tokens][0] = 0;
        batch.logits[batch.n_tokens] = (i == n_tokens - 1);
        batch.n_tokens++;
    }

    if (llama_decode(ctx_, batch) != 0) {
        Logger::Get().Error("Failed to decode prompt");
        llama_batch_free(batch);
        return InferenceResult{};
    }

    for (int i = 0; i < params.max_tokens; i++) {
        float* logits = llama_get_logits(ctx_);
        if (!logits) {
            break;
        }

        int n_vocab = llama_vocab_n_tokens(vocab_);
        llama_token next_token = SampleToken(logits, n_vocab, params.temperature, params.top_p);

        if (next_token == llama_token_eos(vocab_) || next_token == llama_token_eot(vocab_)) {
            break;
        }

        output_tokens.push_back(next_token);

        batch.n_tokens = 0;
        batch.token[0] = next_token;
        batch.pos[0] = n_tokens + i;
        batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0] = true;
        batch.n_tokens = 1;

        if (llama_decode(ctx_, batch) != 0) {
            break;
        }
    }

    llama_batch_free(batch);

    std::string result;
    for (auto token : output_tokens) {
        result += token_to_piece(vocab_, token);
    }

    auto end = std::chrono::high_resolution_clock::now();
    float duration = std::chrono::duration<float, std::milli>(end - start).count();

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

    Logger::Get().Info("Inference complete: " + std::to_string(output_tokens.size()) + " tokens in " +
                       std::to_string(duration) + "ms");

    return InferenceResult{
        .text = result,
        .prompt_tokens = n_tokens,
        .completion_tokens = static_cast<int>(output_tokens.size()),
        .total_tokens = n_tokens + static_cast<int>(output_tokens.size()),
        .duration_ms = duration
    };
}

InferenceResult LlamaEngine::PredictStream(const std::vector<ChatMessage>& messages,
                                               const InferenceParams& params,
                                               TokenCallback on_token) {
    auto start = std::chrono::high_resolution_clock::now();

    llama_memory_clear(llama_get_memory(ctx_), true);

    std::string prompt = BuildPrompt(messages);

    int n_ctx = llama_n_ctx(ctx_);
    std::vector<llama_token> tokens(n_ctx);
    int n_tokens = llama_tokenize(vocab_, prompt.c_str(), static_cast<int>(prompt.size()), tokens.data(), n_ctx, true, false);
    if (n_tokens < 0) {
        n_tokens = -n_tokens;
        tokens.resize(n_tokens);
        llama_tokenize(vocab_, prompt.c_str(), static_cast<int>(prompt.size()), tokens.data(), n_tokens, true, false);
    }

    Logger::Get().Info("Prompt tokenized: " + std::to_string(n_tokens) + " tokens (streaming)");

    std::vector<llama_token> output_tokens;
    output_tokens.reserve(params.max_tokens);

    llama_batch batch = llama_batch_init(512, 0, 1);

    for (int i = 0; i < n_tokens && i < 512; i++) {
        batch.token[batch.n_tokens] = tokens[i];
        batch.pos[batch.n_tokens] = i;
        batch.n_seq_id[batch.n_tokens] = 1;
        batch.seq_id[batch.n_tokens][0] = 0;
        batch.logits[batch.n_tokens] = (i == n_tokens - 1);
        batch.n_tokens++;
    }

    if (llama_decode(ctx_, batch) != 0) {
        Logger::Get().Error("Failed to decode prompt");
        llama_batch_free(batch);
        return InferenceResult{};
    }

    int cumulative = 0;
    for (int i = 0; i < params.max_tokens; i++) {
        float* logits = llama_get_logits(ctx_);
        if (!logits) {
            break;
        }

        int n_vocab = llama_vocab_n_tokens(vocab_);
        llama_token next_token = SampleToken(logits, n_vocab, params.temperature, params.top_p);

        if (next_token == llama_token_eos(vocab_) || next_token == llama_token_eot(vocab_)) {
            break;
        }

        output_tokens.push_back(next_token);

        std::string token_str = token_to_piece(vocab_, next_token);
        if (on_token) {
            on_token(token_str, cumulative++);
        }

        batch.n_tokens = 0;
        batch.token[0] = next_token;
        batch.pos[0] = n_tokens + i;
        batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0] = true;
        batch.n_tokens = 1;

        if (llama_decode(ctx_, batch) != 0) {
            break;
        }
    }

    llama_batch_free(batch);

    std::string result;
    for (auto token : output_tokens) {
        result += token_to_piece(vocab_, token);
    }

    auto end = std::chrono::high_resolution_clock::now();
    float duration = std::chrono::duration<float, std::milli>(end - start).count();

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

    Logger::Get().Info("Streaming inference complete: " + std::to_string(output_tokens.size()) +
                       " tokens in " + std::to_string(duration) + "ms");

    return InferenceResult{
        .text = result,
        .prompt_tokens = n_tokens,
        .completion_tokens = static_cast<int>(output_tokens.size()),
        .total_tokens = n_tokens + static_cast<int>(output_tokens.size()),
        .duration_ms = duration
    };
}

InferenceStats LlamaEngine::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::string LlamaEngine::GetModelName() const {
    if (!gguf_metadata_.model_name.empty()) {
        return gguf_metadata_.model_name;
    }
    if (!model_path_.empty()) {
        std::filesystem::path p(model_path_);
        std::string name = p.filename().string();
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) {
            name = name.substr(0, dot);
        }
        return name;
    }
    return "local-model";
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

llama_token LlamaEngine::SampleToken(const float* logits, int n_vocab, float temperature, float top_p) {
    std::vector<float> probs(n_vocab);
    float max_logit = -1e30f;
    for (int i = 0; i < n_vocab; i++) {
        max_logit = std::max(max_logit, logits[i]);
    }

    float sum_exp = 0.0f;
    for (int i = 0; i < n_vocab; i++) {
        float logit = logits[i];
        if (temperature > 0.0f) {
            logit /= temperature;
        }
        probs[i] = std::exp(logit - max_logit);
        sum_exp += probs[i];
    }

    for (int i = 0; i < n_vocab; i++) {
        probs[i] /= sum_exp;
    }

    std::vector<std::pair<float, int>> sorted_probs;
    sorted_probs.reserve(n_vocab);
    for (int i = 0; i < n_vocab; i++) {
        sorted_probs.push_back({probs[i], i});
    }
    std::sort(sorted_probs.begin(), sorted_probs.end(), std::greater<>());

    float cumulative = 0.0f;
    int cutoff_idx = n_vocab;
    for (int i = 0; i < n_vocab; i++) {
        cumulative += sorted_probs[i].first;
        if (cumulative >= top_p) {
            cutoff_idx = i + 1;
            break;
        }
    }

    float renorm_sum = 0.0f;
    for (int i = 0; i < cutoff_idx; i++) {
        renorm_sum += sorted_probs[i].first;
    }
    for (int i = 0; i < cutoff_idx; i++) {
        sorted_probs[i].first /= renorm_sum;
    }

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

    return sorted_probs[cutoff_idx - 1].second;
}

} // namespace inferdeck::core
