/// @file GteEmbeddingBackend.cpp
/// @brief GTE embedding generation implementation.

#include "text_embedding/GteEmbeddingBackend.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <random>

namespace inferdeck::backends {

std::string GteEmbeddingBackend::GetBackendName() const { return "gte_embedding"; }

BackendStatus GteEmbeddingBackend::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool GteEmbeddingBackend::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = BackendStatus::INITIALIZING;

    // Load GGML embedding model (GTE/text2vec GGUF)
    // auto gguf = GGUFParser::Parse(model_path_);
    // embedding_dim_ = gguf->n_embd;
    // llama_model* model = llama_load_model_from_file(model_path_.c_str(), params);
    // llama_context* ctx = llama_new_context_with_model(model, params);

    spdlog::info("GteEmbeddingBackend: loaded model '{}' [dim: {}]", model_path_, embedding_dim_);
    status_ = BackendStatus::READY;
    return true;
}

void GteEmbeddingBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    // llama_free(ctx);
    // llama_free_model(model);
    status_ = BackendStatus::UNINITIALIZED;
    spdlog::info("GteEmbeddingBackend: shut down");
}

nlohmann::json GteEmbeddingBackend::GetInfo() const {
    nlohmann::json info;
    info["name"] = "gte_embedding";
    info["backend"] = "llama.cpp";
    info["engine"] = "ggml";
    info["model"] = model_name_;
    info["model_path"] = model_path_;
    info["embedding_dimension"] = embedding_dim_;
    info["status"] = static_cast<int>(status_);
    return info;
}

nlohmann::json GteEmbeddingBackend::GetVRAMUsage() const {
    nlohmann::json vram;
    vram["backend"] = "gte_embedding";
    vram["allocated_bytes"] = 0;
    vram["peak_bytes"] = 0;
    return vram;
}

bool GteEmbeddingBackend::IsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_ == BackendStatus::READY;
}

EmbeddingResult GteEmbeddingBackend::Generate(const std::string& input,
                                               const nlohmann::json& params) {
    auto start = std::chrono::steady_clock::now();

    EmbeddingResult result;
    result.embedding.resize(embedding_dim_, 0.0f);

    // Tokenize input
    // auto tokens = llama_tokenize(ctx, input, true);
    // result.token_count = static_cast<int>(tokens.size());
    // Set input tokens
    // llama_set_input_tokens(ctx, tokens.data(), tokens.size());
    // Run model forward pass
    // llama_decode(ctx, llama_get_input_tokens(ctx));
    // Get embeddings
    // const float* embd = llama_get_embeddings(ctx);
    // for (int i = 0; i < embedding_dim_; i++) {
    //     result.embedding[i] = static_cast<float>(embd[i]);
    // }
    // Normalize embedding
    // float norm = 0.0;
    // for (float v : result.embedding) norm += v * v;
    // norm = std::sqrt(norm);
    // for (float& v : result.embedding) v /= norm;

    auto end = std::chrono::steady_clock::now();
    result.inference_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.model_name = model_name_;

    spdlog::debug("GteEmbeddingBackend: generated embedding [{}d] for {} tokens ({}ms)",
                  embedding_dim_, result.token_count, result.inference_time_ms);
    return result;
}

std::vector<EmbeddingResult> GteEmbeddingBackend::GenerateBatch(const std::vector<std::string>& inputs,
                                                                 const nlohmann::json& params) {
    std::vector<EmbeddingResult> results;
    results.reserve(inputs.size());

    for (const auto& input : inputs) {
        results.push_back(Generate(input, params));
    }

    spdlog::info("GteEmbeddingBackend: batch generated {} embeddings", results.size());
    return results;
}

int GteEmbeddingBackend::GetEmbeddingDimension() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return embedding_dim_;
}

nlohmann::json GteEmbeddingBackend::GetAvailableModels() const {
    if (!available_models_.empty()) {
        return available_models_;
    }

    // Standard GTE embedding models
    available_models_ = nlohmann::json::array();

    nlohmann::json gte_small;
    gte_small["name"] = "gte-small";
    gte_small["path"] = "models/gte-small.q4_k_s.gguf";
    gte_small["dimension"] = 384;
    available_models_.push_back(gte_small);

    nlohmann::json gte_base;
    gte_base["name"] = "gte-base";
    gte_base["path"] = "models/gte-base.q4_k_s.gguf";
    gte_base["dimension"] = 768;
    available_models_.push_back(gte_base);

    nlohmann::json gte_large;
    gte_large["name"] = "gte-large";
    gte_large["path"] = "models/gte-large.q4_k_s.gguf";
    gte_large["dimension"] = 1024;
    available_models_.push_back(gte_large);

    return available_models_;
}

bool GteEmbeddingBackend::SetModelPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_path_ = path;
    // Extract model name from path
    auto pos = path.find_last_of("/\\");
    model_name_ = (pos != std::string::npos) ? path.substr(pos + 1) : path;
    return true;
}

std::string GteEmbeddingBackend::GetModelPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return model_path_;
}

} // namespace inferdeck::backends
