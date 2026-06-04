#include "llama_cpp/LlamaEngine.hpp"
#include "llama_cpp/LlamaServerManager.hpp"
#include "core/Logger.hpp"

#include "llama.h"
#include "ggml-backend.h"
#include "chat.h"
#include "mtmd.h"
#include "mtmd-helper.h"

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

std::string DetectModelFamily(const std::string& model_name) {
    auto lower = model_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.find("qwen") != std::string::npos) return "qwen";
    if (lower.find("gpt-oss") != std::string::npos || lower.find("harmony") != std::string::npos) return "harmony";
    return "universal";
}

common_chat_msg ToCommonChatMsg(const ChatMessage& msg) {
    common_chat_msg cmsg;
    cmsg.role = RoleToTemplateRole(msg.role);
    cmsg.content = msg.content;
    cmsg.tool_call_id = msg.tool_call_id;

    if (msg.role == MessageRole::Assistant && !msg.tool_calls_json.empty()) {
        try {
            auto calls = nlohmann::json::parse(msg.tool_calls_json);
            if (calls.is_array()) {
                for (const auto& call : calls) {
                    common_chat_tool_call tc;
                    if (call.contains("function")) {
                        const auto& fn = call["function"];
                        tc.name = fn.value("name", "");
                        if (fn.contains("arguments")) {
                            const auto& a = fn["arguments"];
                            tc.arguments = a.is_string() ? a.get<std::string>() : a.dump();
                        }
                    } else if (call.contains("name")) {
                        tc.name = call.value("name", "");
                        if (call.contains("arguments")) {
                            const auto& a = call["arguments"];
                            tc.arguments = a.is_string() ? a.get<std::string>() : a.dump();
                        }
                    }
                    tc.id = call.value("id", "");
                    if (!tc.name.empty()) cmsg.tool_calls.push_back(std::move(tc));
                }
            }
        } catch (...) {}
    }

    if (msg.role == MessageRole::Tool) {
        cmsg.content = msg.content;
        cmsg.tool_call_id = msg.tool_call_id;
    }

    return cmsg;
}

std::vector<common_chat_tool> ParseToolsJson(const std::string& tools_json) {
    std::vector<common_chat_tool> tools;
    if (tools_json.empty()) return tools;
    try {
        auto j = nlohmann::json::parse(tools_json);
        if (!j.is_array()) return tools;
        for (const auto& item : j) {
            common_chat_tool tool;
            if (item.contains("function")) {
                const auto& fn = item["function"];
                tool.name = fn.value("name", "");
                tool.description = fn.value("description", "");
                if (fn.contains("parameters")) {
                    tool.parameters = fn["parameters"].dump();
                }
            } else {
                tool.name = item.value("name", "");
                tool.description = item.value("description", "");
                if (item.contains("parameters")) {
                    tool.parameters = item["parameters"].dump();
                }
            }
            if (!tool.name.empty()) tools.push_back(std::move(tool));
        }
    } catch (...) {}
    return tools;
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

ggml_type CacheTypeFromString(const std::string& type) {
    if (type == "f32")  return GGML_TYPE_F32;
    if (type == "f16")  return GGML_TYPE_F16;
    if (type == "q8_0") return GGML_TYPE_Q8_0;
    if (type == "q4_0") return GGML_TYPE_Q4_0;
    if (type == "q4_1") return GGML_TYPE_Q4_1;
    if (type == "q5_0") return GGML_TYPE_Q5_0;
    if (type == "q5_1") return GGML_TYPE_Q5_1;
    if (type == "q2_K") return GGML_TYPE_Q2_K;
    if (type == "q3_K") return GGML_TYPE_Q3_K;
    if (type == "q4_K") return GGML_TYPE_Q4_K;
    if (type == "q5_K") return GGML_TYPE_Q5_K;
    if (type == "q6_K") return GGML_TYPE_Q6_K;
    if (type == "q8_K") return GGML_TYPE_Q8_K;
    return GGML_TYPE_F16;
}

llama_sampler* CreateSampler(const InferenceParams& params, llama_model* model) {
    auto sparams = llama_sampler_chain_default_params();
    sparams.no_perf = true;
    llama_sampler* sampler = llama_sampler_chain_init(sparams);

    // Order matches llama-server: penalties -> DRY -> top_k -> top_p -> min_p -> temp -> dist
    llama_sampler_chain_add(sampler, llama_sampler_init_penalties(
        params.penalty_last_n,
        params.repetition_penalty,
        params.frequency_penalty,
        params.presence_penalty));

    if (params.dry_multiplier > 0.0f && model) {
        const char* dry_seq_breakers[] = {"\n", ":", "\"", "*"};
        llama_sampler_chain_add(sampler, llama_sampler_init_dry(
            llama_model_get_vocab(model),
            llama_model_n_ctx_train(model),
            params.dry_multiplier, params.dry_base, params.dry_allowed_length, params.dry_penalty_last_n,
            dry_seq_breakers, 4));
    }

    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(std::max(1, static_cast<int>(params.top_k))));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(std::clamp(params.top_p, 0.0f, 1.0f), 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_min_p(std::clamp(params.min_p, 0.0f, 1.0f), 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(std::max(0.0f, params.temperature)));

    uint32_t seed = params.seed >= 0 ? static_cast<uint32_t>(params.seed) : LLAMA_DEFAULT_SEED;
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(seed));
    return sampler;
}

bool EndsWithStop(const std::string& text, const std::vector<std::string>& stops) {
    for (const auto& stop : stops) {
        if (stop.empty()) continue;
        if (text.size() >= stop.size() && text.compare(text.size() - stop.size(), stop.size(), stop) == 0) {
            return true;
        }
    }
    return false;
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
                             int context_size,
                             const std::string& mmproj_path) {
    EngineParams params;
    params.model_path = model_path;
    params.precision = precision;
    params.gpu_layers = gpu_layers;
    params.context_size = context_size;
    params.mmproj_path = mmproj_path;
    return Initialize(params);
}

bool LlamaEngine::Initialize(const EngineParams& params) {
    std::lock_guard<std::mutex> generation_lock(generation_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);

    std::call_once(g_llama_backend_once, [] {
        llama_backend_init();
        ggml_backend_load_all();
    });

    if (!std::filesystem::exists(params.model_path)) {
        Logger::Get().Error("Model file not found: " + params.model_path);
        return false;
    }

    FreeRuntime();

    last_message_count_ = 0;
    last_prompt_token_count_ = 0;
    last_n_past_ = 0;
    cache_valid_ = false;

    engine_params_ = params;
    abort_requested_.store(false);

    std::filesystem::path p(params.model_path);
    current_model_name_ = p.stem().string();

    Logger::Get().Info("Loading internal llama.cpp Vulkan runtime with model: " + params.model_path);
    Logger::Get().Info("GPU layers: " + std::string(params.gpu_layers < 0 ? "all" : std::to_string(params.gpu_layers)));
    Logger::Get().Info("Context size: " + std::to_string(params.context_size));

    if (params.n_threads > 0) {
        Logger::Get().Info("Threads: " + std::to_string(params.n_threads) + " (gen) / " +
                           std::to_string(params.n_threads_batch > 0 ? params.n_threads_batch : params.n_threads) + " (batch)");
    }

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = params.gpu_layers < 0 ? 999 : params.gpu_layers;
    model_params.split_mode = LLAMA_SPLIT_MODE_NONE;
    model_params.main_gpu = 0;
    model_params.use_mmap = false;

    llama_model* model = llama_model_load_from_file(params.model_path.c_str(), model_params);
    if (!model) {
        Logger::Get().Error("Failed to load model through internal llama.cpp runtime");
        return false;
    }

    // Initialize chat templates from the model
    common_chat_templates_free(static_cast<common_chat_templates*>(chat_templates_));
    chat_templates_ = common_chat_templates_init(model, "").release();

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = static_cast<uint32_t>(std::max(512, params.context_size));
    ctx_params.n_batch = static_cast<uint32_t>(params.batch_size);
    ctx_params.n_ubatch = static_cast<uint32_t>(params.batch_size);
    ctx_params.n_seq_max = 1;
    ctx_params.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    ctx_params.offload_kqv = true;
    ctx_params.kv_unified = false;
    ctx_params.swa_full = params.swa_full;
    ctx_params.op_offload = params.op_offload;
    ctx_params.no_perf = params.no_perf;
    if (params.n_threads > 0) {
        ctx_params.n_threads = params.n_threads;
    }
    if (params.n_threads_batch > 0) {
        ctx_params.n_threads_batch = params.n_threads_batch;
    } else if (params.n_threads > 0) {
        ctx_params.n_threads_batch = params.n_threads;
    }
    ctx_params.type_k = CacheTypeFromString(params.cache_type_k);
    ctx_params.type_v = CacheTypeFromString(params.cache_type_v);

    llama_context* context = llama_init_from_model(model, ctx_params);
    if (!context) {
        llama_model_free(model);
        Logger::Get().Error("Failed to create llama.cpp context");
        return false;
    }

    model_ = model;
    context_ = context;
    initialized_ = true;

    {
        auto try_mmproj = [&](const std::string& path) -> bool {
            auto mp = mtmd_context_params_default();
            mp.use_gpu = true;
            mmproj_ctx_ = mtmd_init_from_file(path.c_str(), static_cast<llama_model*>(model), mp);
            if (mmproj_ctx_) {
                has_vision_ = true;
                Logger::Get().Info("Vision model initialized with mmproj: " + path);
                return true;
            }
            return false;
        };

        bool mmproj_loaded = false;
        if (!params.mmproj_path.empty()) {
            mmproj_loaded = try_mmproj(params.mmproj_path);
        }
        if (!mmproj_loaded) {
            std::filesystem::path model_dir = std::filesystem::path(params.model_path).parent_path();
            if (std::filesystem::exists(model_dir)) {
                for (const auto& entry : std::filesystem::directory_iterator(model_dir)) {
                    if (!entry.is_regular_file()) continue;
                    std::string filename = entry.path().filename().string();
                    if (filename.find("mmproj") == 0 && entry.path().extension() == ".gguf") {
                        if (try_mmproj(entry.path().string())) {
                            mmproj_loaded = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!mmproj_loaded && !params.mmproj_path.empty()) {
            Logger::Get().Warn("Failed to initialize vision model from mmproj: " + params.mmproj_path);
        }
    }

    LlamaServerManager::Get().Start(params.model_path, params.gpu_layers, params.context_size, 0);
    Logger::Get().Info("LlamaEngine initialized with in-process llama.cpp runtime");
    return true;
}

bool LlamaEngine::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

bool LlamaEngine::SwitchModel(const std::string& model_path) {
    EngineParams params;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_ && !engine_params_.model_path.empty()) {
            bool same_model = engine_params_.model_path == model_path;
            if (!same_model) {
                try {
                    same_model = std::filesystem::equivalent(engine_params_.model_path, model_path);
                } catch (...) {
                    same_model = false;
                }
            }
            if (same_model) {
                Logger::Get().Info("Requested model is already loaded; keeping current llama.cpp context");
                return true;
            }
        }
        params = engine_params_;
    }
    params.model_path = model_path;
    return Initialize(params);
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

    // Use common_chat_templates_apply for prompt building
    common_chat_templates_inputs inputs;
    inputs.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
    for (const auto& msg : messages) {
        inputs.messages.push_back(ToCommonChatMsg(msg));
    }
    auto tools = ParseToolsJson(params.tools_json);
    if (!tools.empty()) {
        inputs.tools = tools;
        inputs.tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;
    }

    auto chat_params = common_chat_templates_apply(static_cast<common_chat_templates*>(chat_templates_), inputs);
    std::string prompt = chat_params.prompt;
    if (!chat_params.generation_prompt.empty()) {
        prompt += chat_params.generation_prompt;
    }

    bool reuse_cache = cache_valid_ &&
                       messages.size() > last_message_count_ &&
                       last_message_count_ > 0;

    bool has_images = false;
    for (const auto& m : messages) {
        if (!m.images.empty()) { has_images = true; break; }
    }

    bool multimodal_used = false;
    std::vector<llama_token> prompt_tokens;
    std::vector<mtmd_bitmap*> owned_bitmaps;
    mtmd_input_chunks* mm_chunks = nullptr;
    int n_past = 0;

    if (has_vision_ && has_images) {
        llama_memory_clear(llama_get_memory(context), true);
        n_past = 0;
        reuse_cache = false;
        Logger::Get().Info("Building multimodal prompt with " + std::to_string(messages.size()) + " messages");

        std::vector<const mtmd_bitmap*> bitmaps;
        for (const auto& m : messages) {
            for (const auto& img : m.images) {
                auto* bmp = mtmd_helper_bitmap_init_from_buf(
                    static_cast<mtmd_context*>(mmproj_ctx_),
                    img.data(), img.size());
                if (bmp) {
                    bitmaps.push_back(bmp);
                    owned_bitmaps.push_back(bmp);
                }
            }
        }

        if (!bitmaps.empty()) {
            mm_chunks = mtmd_input_chunks_init();
            mtmd_input_text mm_text;
            mm_text.text = prompt.c_str();
            mm_text.add_special = true;
            mm_text.parse_special = false;

            int32_t tz_res = mtmd_tokenize(static_cast<mtmd_context*>(mmproj_ctx_),
                                          mm_chunks, &mm_text,
                                          bitmaps.data(), bitmaps.size());
            if (tz_res == 0) {
                size_t total_tokens = 0;
                for (size_t i = 0; i < mtmd_input_chunks_size(mm_chunks); ++i) {
                    total_tokens += mtmd_input_chunk_get_n_tokens(mtmd_input_chunks_get(mm_chunks, i));
                }
                Logger::Get().Info("Multimodal prompt tokenized: " + std::to_string(total_tokens) + " tokens in " +
                                   std::to_string(mtmd_input_chunks_size(mm_chunks)) + " chunks");

                llama_pos mm_n_past = 0;
                int eval_res = mtmd_helper_eval_chunks(
                    static_cast<mtmd_context*>(mmproj_ctx_),
                    context,
                    mm_chunks,
                    mm_n_past,
                    0, 512, true, &mm_n_past);
                if (eval_res == 0) {
                    multimodal_used = true;
                    result.prompt_tokens = static_cast<int>(total_tokens);
                    n_past = static_cast<int>(mm_n_past);
                    Logger::Get().Info("Multimodal prompt evaluated into KV cache, n_past=" + std::to_string(mm_n_past));
                } else {
                    Logger::Get().Error("mtmd_helper_eval_chunks failed: " + std::to_string(eval_res));
                }
            } else {
                Logger::Get().Warn("mtmd_tokenize failed with code " + std::to_string(tz_res));
            }
        }

        if (mm_chunks) {
            mtmd_input_chunks_free(mm_chunks);
            mm_chunks = nullptr;
        }
        for (auto* bmp : owned_bitmaps) {
            mtmd_bitmap_free(bmp);
        }
        owned_bitmaps.clear();

        if (!multimodal_used) {
            prompt_tokens = Tokenize(model, prompt, true);
        }
    } else {
        prompt_tokens = Tokenize(model, prompt, true);
    }
    if (prompt_tokens.empty() && !multimodal_used) {
        result.http_status = 500;
        result.error_message = "Failed to tokenize prompt";
        return result;
    }

    const int32_t model_max_ctx = llama_model_n_ctx_train(model);
    const int max_predict = params.max_tokens > 0 ? params.max_tokens : std::min(model_max_ctx, 8192);
    Logger::Get().Info("max_predict: " + std::to_string(max_predict) + " (client requested: " + std::to_string(params.max_tokens) + ")");
    if (!multimodal_used) {
        result.prompt_tokens = static_cast<int>(prompt_tokens.size());
    }

    int start_offset = 0;
    if (!multimodal_used) {
        if (reuse_cache && prompt_tokens.size() >= static_cast<std::size_t>(last_prompt_token_count_)) {
            start_offset = last_prompt_token_count_;
            llama_memory_seq_rm(llama_get_memory(context), 0, start_offset, -1);
            n_past = start_offset;
        } else {
            llama_memory_clear(llama_get_memory(context), true);
            start_offset = 0;
            n_past = 0;
        }
    }

    llama_sampler* sampler = CreateSampler(params, model);

    // Add grammar sampler if one was provided by common_chat_templates_apply
    // Skip lazy grammars (tool-call grammars that need trigger-token activation)
    if (!chat_params.grammar.empty() && !chat_params.grammar_lazy) {
        llama_sampler* grammar_sampler = llama_sampler_init_grammar(
            llama_model_get_vocab(model),
            chat_params.grammar.c_str(),
            "root");
        llama_sampler_chain_add(sampler, grammar_sampler);
    }

    constexpr int32_t max_batch_tokens = 512;
    int generated_tokens = 0;
    int decode_status = 0;
    auto eval_tokens = [&](const std::vector<llama_token>& tokens, bool encode, std::size_t start_offset = 0) {
        for (std::size_t offset = start_offset; offset < tokens.size() && decode_status == 0; offset += max_batch_tokens) {
            const auto remaining = tokens.size() - offset;
            const auto chunk_size = static_cast<int32_t>(std::min<std::size_t>(remaining, max_batch_tokens));
            llama_batch batch = llama_batch_get_one(const_cast<llama_token*>(tokens.data() + offset), chunk_size);
            decode_status = encode ? llama_encode(context, batch) : llama_decode(context, batch);
            n_past += batch.n_tokens;
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
        eval_tokens(prompt_tokens, true, start_offset);
        if (decode_status == 0) {
            llama_token decoder_start = llama_model_decoder_start_token(model);
            if (decoder_start == LLAMA_TOKEN_NULL) decoder_start = llama_vocab_bos(llama_model_get_vocab(model));
            batch = llama_batch_get_one(&decoder_start, 1);
        }
    } else {
        eval_tokens(prompt_tokens, false, start_offset);
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
        n_past += batch.n_tokens;

        if (!sample_next(next_token)) break;
        batch = llama_batch_get_one(&next_token, 1);
    }

    llama_sampler_free(sampler);
    // grammar_sampler is owned by the chain, no need to free separately

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

    if (!result.HasError() && !chat_params.generation_prompt.empty()) {
        result.text = chat_params.generation_prompt + result.text;
    }

    last_message_count_ = messages.size();
    last_n_past_ = n_past;
    last_prompt_token_count_ = static_cast<int>(prompt_tokens.size());
    cache_valid_ = !result.HasError();

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

std::string LlamaEngine::GetModelFamily() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return DetectModelFamily(current_model_name_);
}

std::string LlamaEngine::GetPrecision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return engine_params_.precision;
}

void LlamaEngine::AbortActiveRequest(const std::string& reason) {
    Logger::Get().Warn("Aborting active internal llama.cpp request: " + reason);
    abort_requested_.store(true);
}

void LlamaEngine::FreeRuntime() {
    common_chat_templates_free(static_cast<common_chat_templates*>(chat_templates_));
    chat_templates_ = nullptr;
    if (mmproj_ctx_) {
        mtmd_free(static_cast<mtmd_context*>(mmproj_ctx_));
        mmproj_ctx_ = nullptr;
        has_vision_ = false;
    }
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