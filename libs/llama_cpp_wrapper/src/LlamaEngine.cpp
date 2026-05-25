#include "llama_cpp/LlamaEngine.hpp"
#include "llama_cpp/LlamaServerManager.hpp"
#include "core/Logger.hpp"

#include "llama.h"
#include "ggml-backend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace inferdeck::core {
namespace {

std::once_flag g_llama_backend_once;

llama_model* AsModel(void* model) {
    return static_cast<llama_model*>(model);
}

llama_context* AsContext(void* context) {
    return static_cast<llama_context*>(context);
}

std::string RoleToTemplateRole(MessageRole role) {
    switch (role) {
        case MessageRole::System: return "system";
        case MessageRole::Assistant: return "assistant";
        case MessageRole::Tool: return "tool";
        case MessageRole::User:
        default: return "user";
    }
}

std::string BuildToolInstruction(const std::string& tools_json) {
    if (tools_json.empty()) return {};
    return
        "Tools are available. When a tool is needed, emit exactly one machine-readable tool call and no explanatory text. "
        "For Qwen-style models, use <tool_call>{\"name\":\"read\",\"arguments\":{\"filePath\":\"src/app/page.tsx\"}}</tool_call>. "
        "For GPT-OSS/Harmony-style models, use <|channel|>commentary to=tool.read <|constrain|>json<|message|>{\"filePath\":\"src/app/page.tsx\"}<|call|>. "
        "Do not write plans, markdown, literal 'tool_calls:' prose, or assistant_tool_calls_json text when a tool is needed. "
        "Available tools JSON: " + tools_json;
}

std::string BuildFallbackPrompt(const std::vector<ChatMessage>& messages,
                                const InferenceParams& params) {
    std::ostringstream prompt;
    auto tool_instruction = BuildToolInstruction(params.tools_json);
    if (!tool_instruction.empty()) {
        prompt << "<|system|>\n" << tool_instruction << "\n";
    }
    for (const auto& msg : messages) {
        prompt << "<|" << RoleToTemplateRole(msg.role) << "|>\n";
        if (!msg.name.empty()) prompt << "name: " << msg.name << "\n";
        if (!msg.tool_call_id.empty()) prompt << "tool_call_id: " << msg.tool_call_id << "\n";
        prompt << msg.content << "\n";
    }
    prompt << "<|assistant|>\n";
    return prompt.str();
}

std::string BuildChatPrompt(llama_model* model,
                            const std::vector<ChatMessage>& messages,
                            const InferenceParams& params) {
    std::vector<std::string> roles;
    std::vector<std::string> contents;
    roles.reserve(messages.size() + 1);
    contents.reserve(messages.size() + 1);

    auto tool_instruction = BuildToolInstruction(params.tools_json);
    if (!tool_instruction.empty()) {
        roles.push_back("system");
        contents.push_back(tool_instruction);
    }

    for (const auto& msg : messages) {
        roles.push_back(RoleToTemplateRole(msg.role));
        std::string content;
        if (!msg.name.empty()) content += "name: " + msg.name + "\n";
        if (!msg.tool_call_id.empty()) content += "tool_call_id: " + msg.tool_call_id + "\n";
        content += msg.content;
        contents.push_back(std::move(content));
    }

    std::vector<llama_chat_message> chat;
    chat.reserve(roles.size());
    for (std::size_t i = 0; i < roles.size(); ++i) {
        chat.push_back({roles[i].c_str(), contents[i].c_str()});
    }

    const char* tmpl = model ? llama_model_chat_template(model, nullptr) : nullptr;
    int32_t needed = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, nullptr, 0);
    if (needed <= 0) return BuildFallbackPrompt(messages, params);

    std::string prompt(static_cast<std::size_t>(needed), '\0');
    int32_t written = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, prompt.data(), needed);
    if (written < 0) return BuildFallbackPrompt(messages, params);
    prompt.resize(static_cast<std::size_t>(written));
    return prompt;
}

std::vector<llama_token> Tokenize(llama_model* model, const std::string& text, bool add_bos) {
    const llama_vocab* vocab = llama_model_get_vocab(model);
    int32_t count = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()), nullptr, 0, add_bos, true);
    if (count < 0) count = -count;
    std::vector<llama_token> tokens(static_cast<std::size_t>(count));
    if (tokens.empty()) return tokens;
    int32_t actual = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()), tokens.data(), count, add_bos, true);
    if (actual < 0) {
        tokens.clear();
        return tokens;
    }
    tokens.resize(static_cast<std::size_t>(actual));
    return tokens;
}

std::string TokenToPiece(llama_model* model, llama_token token) {
    const llama_vocab* vocab = llama_model_get_vocab(model);
    std::vector<char> buffer(128);
    int32_t written = llama_token_to_piece(vocab, token, buffer.data(), static_cast<int32_t>(buffer.size()), 0, false);
    if (written < 0) {
        buffer.resize(static_cast<std::size_t>(-written));
        written = llama_token_to_piece(vocab, token, buffer.data(), static_cast<int32_t>(buffer.size()), 0, false);
    }
    if (written <= 0) return {};
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

llama_sampler* CreateSampler(const InferenceParams& params) {
    auto sparams = llama_sampler_chain_default_params();
    sparams.no_perf = false;
    llama_sampler* sampler = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(std::clamp(params.top_p, 0.0f, 1.0f), 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(std::max(0.0f, params.temperature)));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(std::random_device{}()));
    return sampler;
}

bool EndsWithStop(const std::string& text, const std::string& stop) {
    if (stop.empty() || text.size() < stop.size()) return false;
    return text.compare(text.size() - stop.size(), stop.size(), stop) == 0;
}

} // namespace

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
    std::lock_guard<std::mutex> generation_lock(generation_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);

    std::call_once(g_llama_backend_once, [] {
        llama_backend_init();
        ggml_backend_load_all();
    });

    if (!std::filesystem::exists(model_path)) {
        Logger::Get().Error("Model file not found: " + model_path);
        return false;
    }

    FreeRuntime();

    model_path_ = model_path;
    precision_ = precision;
    gpu_layers_ = gpu_layers;
    context_size_ = context_size;
    abort_requested_.store(false);

    std::filesystem::path p(model_path);
    current_model_name_ = p.stem().string();

    Logger::Get().Info("Loading internal llama.cpp Vulkan runtime with model: " + model_path);
    Logger::Get().Info("GPU layers: " + std::string(gpu_layers < 0 ? "all" : std::to_string(gpu_layers)));
    Logger::Get().Info("Context size: " + std::to_string(context_size));

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = gpu_layers < 0 ? 999 : gpu_layers;
    model_params.split_mode = LLAMA_SPLIT_MODE_NONE;
    model_params.main_gpu = 0;
    model_params.use_mmap = false;

    llama_model* model = llama_model_load_from_file(model_path.c_str(), model_params);
    if (!model) {
        Logger::Get().Error("Failed to load model through internal llama.cpp runtime");
        return false;
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = static_cast<uint32_t>(std::max(512, context_size));
    ctx_params.n_batch = 512;
    ctx_params.n_ubatch = 512;
    ctx_params.n_seq_max = 1;
    ctx_params.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    ctx_params.offload_kqv = true;
    ctx_params.kv_unified = true;
    ctx_params.no_perf = false;

    llama_context* context = llama_init_from_model(model, ctx_params);
    if (!context) {
        llama_model_free(model);
        Logger::Get().Error("Failed to create llama.cpp context");
        return false;
    }

    model_ = model;
    context_ = context;
    initialized_ = true;
    LlamaServerManager::Get().Start(model_path, gpu_layers, context_size, 0);
    Logger::Get().Info("LlamaEngine initialized with in-process llama.cpp runtime");
    return true;
}

bool LlamaEngine::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

bool LlamaEngine::SwitchModel(const std::string& model_path) {
    std::string precision;
    int gpu_layers = -1;
    int context_size = 100000;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_ && !model_path_.empty()) {
            bool same_model = model_path_ == model_path;
            if (!same_model) {
                try {
                    same_model = std::filesystem::equivalent(model_path_, model_path);
                } catch (...) {
                    same_model = false;
                }
            }
            if (same_model) {
                Logger::Get().Info("Requested model is already loaded; keeping current llama.cpp context");
                return true;
            }
        }
        precision = precision_.empty() ? "auto" : precision_;
        gpu_layers = gpu_layers_;
        context_size = context_size_;
    }
    return Initialize(model_path, precision, gpu_layers, context_size);
}

bool LlamaEngine::LoadModel(const std::string& model_path) {
    return SwitchModel(model_path);
}

std::string LlamaEngine::role_to_string(MessageRole role) const {
    return RoleToTemplateRole(role);
}

InferenceResult LlamaEngine::Generate(const std::vector<ChatMessage>& messages,
                                      const InferenceParams& params,
                                      TokenCallback on_token,
                                      StreamHeartbeatCallback on_heartbeat) {
    std::lock_guard<std::mutex> generation_lock(generation_mutex_);
    auto started_at = std::chrono::high_resolution_clock::now();

    llama_model* model = nullptr;
    llama_context* context = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        model = AsModel(model_);
        context = AsContext(context_);
    }

    InferenceResult result;
    if (!model || !context) {
        result.http_status = 503;
        result.error_message = "Internal llama.cpp runtime is not initialized";
        return result;
    }

    abort_requested_.store(false);
    llama_memory_clear(llama_get_memory(context), true);

    const std::string prompt = BuildChatPrompt(model, messages, params);
    auto prompt_tokens = Tokenize(model, prompt, true);
    if (prompt_tokens.empty()) {
        result.http_status = 500;
        result.error_message = "Failed to tokenize prompt";
        return result;
    }

    const int max_predict = params.max_tokens > 0 ? params.max_tokens : 1024;
    result.prompt_tokens = static_cast<int>(prompt_tokens.size());

    llama_sampler* sampler = CreateSampler(params);
    constexpr int32_t max_batch_tokens = 512;
    int generated_tokens = 0;
    int decode_status = 0;
    int n_pos = 0;

    auto eval_tokens = [&](const std::vector<llama_token>& tokens, bool encode) {
        for (std::size_t offset = 0; offset < tokens.size() && decode_status == 0; offset += max_batch_tokens) {
            const auto remaining = tokens.size() - offset;
            const auto chunk_size = static_cast<int32_t>(std::min<std::size_t>(remaining, max_batch_tokens));
            llama_batch batch = llama_batch_get_one(const_cast<llama_token*>(tokens.data() + offset), chunk_size);
            decode_status = encode ? llama_encode(context, batch) : llama_decode(context, batch);
            n_pos += batch.n_tokens;
            if (on_heartbeat) on_heartbeat();
        }
    };

    auto sample_next = [&](llama_token& token) {
        token = llama_sampler_sample(sampler, context, -1);
        const llama_vocab* vocab = llama_model_get_vocab(model);
        if (llama_vocab_is_eog(vocab, token)) return false;

        ++generated_tokens;
        if (!llama_vocab_is_control(vocab, token)) {
            std::string piece = TokenToPiece(model, token);
            if (!piece.empty()) {
                result.text += piece;
                if (on_token) on_token(piece, TokenType::Content, generated_tokens);
                if (EndsWithStop(result.text, params.stop)) return false;
            }
        }
        if (on_heartbeat && generated_tokens % 32 == 0) on_heartbeat();
        return generated_tokens < max_predict;
    };

    llama_token next_token = LLAMA_TOKEN_NULL;
    llama_batch batch{};

    if (llama_model_has_encoder(model)) {
        eval_tokens(prompt_tokens, true);
        if (decode_status == 0) {
            llama_token decoder_start = llama_model_decoder_start_token(model);
            if (decoder_start == LLAMA_TOKEN_NULL) decoder_start = llama_vocab_bos(llama_model_get_vocab(model));
            batch = llama_batch_get_one(&decoder_start, 1);
        }
    } else {
        eval_tokens(prompt_tokens, false);
        if (decode_status == 0) {
            if (max_predict > 0 && sample_next(next_token)) {
                batch = llama_batch_get_one(&next_token, 1);
            } else {
                generated_tokens = max_predict;
            }
        }
    }

    for (; decode_status == 0 && generated_tokens < max_predict;) {
        if (abort_requested_.load()) {
            result.error_message = "Generation aborted";
            break;
        }

        decode_status = llama_decode(context, batch);
        if (decode_status != 0) break;
        n_pos += batch.n_tokens;

        if (!sample_next(next_token)) break;
        batch = llama_batch_get_one(&next_token, 1);
    }

    llama_sampler_free(sampler);

    if (decode_status != 0 && result.error_message.empty()) {
        result.http_status = 500;
        result.error_message = "llama_decode failed: " + std::to_string(decode_status);
    }

    auto finished_at = std::chrono::high_resolution_clock::now();
    result.duration_ms = std::chrono::duration<float, std::milli>(finished_at - started_at).count();
    result.completion_tokens = generated_tokens;
    result.total_tokens = result.prompt_tokens + result.completion_tokens;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.total_requests++;
        if (result.HasError()) {
            stats_.failed_requests++;
        } else {
            stats_.successful_requests++;
            stats_.avg_latency_ms = (stats_.avg_latency_ms * (stats_.successful_requests - 1) + result.duration_ms) / stats_.successful_requests;
            stats_.tokens_generated += result.completion_tokens;
            stats_.tokens_processed += result.prompt_tokens;
        }
        stats_.max_latency_ms = std::max(stats_.max_latency_ms, result.duration_ms);
        stats_.min_latency_ms = std::min(stats_.min_latency_ms, result.duration_ms);
    }

    return result;
}

InferenceResult LlamaEngine::Predict(const std::vector<ChatMessage>& messages,
                                     const InferenceParams& params) {
    auto result = Generate(messages, params, nullptr, nullptr);
    if (result.HasError()) {
        Logger::Get().Error("Internal llama.cpp inference failed: " + result.error_message);
    }
    return result;
}

InferenceResult LlamaEngine::PredictStream(const std::vector<ChatMessage>& messages,
                                           const InferenceParams& params,
                                           TokenCallback on_token,
                                           StreamHeartbeatCallback on_heartbeat) {
    auto result = Generate(messages, params, std::move(on_token), std::move(on_heartbeat));
    if (result.HasError()) {
        Logger::Get().Error("Internal llama.cpp stream inference failed: " + result.error_message);
    }
    return result;
}

InferenceStats LlamaEngine::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::string LlamaEngine::GetModelName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_model_name_;
}

std::string LlamaEngine::GetPrecision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return precision_;
}

void LlamaEngine::AbortActiveRequest(const std::string& reason) {
    Logger::Get().Warn("Aborting active internal llama.cpp request: " + reason);
    abort_requested_.store(true);
}

void LlamaEngine::FreeRuntime() {
    if (context_) {
        llama_free(AsContext(context_));
        context_ = nullptr;
    }
    if (model_) {
        llama_model_free(AsModel(model_));
        model_ = nullptr;
    }
}

void LlamaEngine::Shutdown() {
    std::lock_guard<std::mutex> generation_lock(generation_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ && !model_ && !context_) return;

    abort_requested_.store(true);
    FreeRuntime();
    initialized_ = false;
    LlamaServerManager::Get().Stop();
    Logger::Get().Info("LlamaEngine shut down internal llama.cpp runtime");
}

GpuInfo LlamaEngine::GetGpuInfo() const {
    GpuInfo info;
    info.name = "Internal llama.cpp Vulkan";
    info.is_discrete = true;
    return info;
}

} // namespace inferdeck::core
