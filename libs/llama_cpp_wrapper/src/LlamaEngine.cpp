#include "llama_cpp/LlamaEngine.hpp"
#include "llama_cpp/LlamaServerManager.hpp"
#include "core/Logger.hpp"

#include "llama.h"
#include "ggml-backend.h"
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

std::string DetectModelFamily(const std::string& model_name) {
    auto lower = model_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.find("qwen") != std::string::npos) return "qwen";
    if (lower.find("gpt-oss") != std::string::npos || lower.find("harmony") != std::string::npos) return "harmony";
    return "universal";
}

std::string BuildToolInstruction(const std::string& tools_json, const std::string& model_family) {
    if (tools_json.empty()) return {};
    std::string family = model_family.empty() ? "universal" : model_family;
    if (family == "qwen") {
        return
            "# Tools\n\n"
            "You have access to the following functions:\n\n"
            "<tools>\n"
            + tools_json + "\n"
            "</tools>\n\n"
            "If you choose to call a function ONLY reply in the following format with NO suffix:\n\n"
            "<tool_call>\n"
            "<function=example_function_name>\n"
            "<parameter=example_parameter_1>\n"
            "value_1\n"
            "</parameter>\n"
            "<parameter=example_parameter_2>\n"
            "This is the value for the second parameter\n"
            "that can span\n"
            "multiple lines\n"
            "</parameter>\n"
            "</function>\n"
            "</tool_call>\n\n"
            "<IMPORTANT>\n"
            "Reminder:\n"
            "- Function calls MUST follow the specified format: an inner <function=...></function> block must be nested within <tool_call></tool_call> XML tags\n"
            "- Required parameters MUST be specified\n"
            "- You may provide optional reasoning for your function call in natural language BEFORE the function call, but NOT after\n"
            "- If there is no function call available, answer the question like normal with your current knowledge and do not tell the user about function calls\n"
            "</IMPORTANT>";
    }
    if (family == "harmony") {
        return
            "Tools are available. When a tool is needed, emit exactly one machine-readable tool call in Harmony format. "
            "Format: <|channel|>commentary to=tool_name <|constrain|>json<|message|>{\"arg\":\"value\"}<|call|>. "
            "After receiving tool output, either emit the next tool call or provide the final answer. "
            "Do not narrate future tool use such as 'I'll read', 'let me inspect', or 'continue reading'; call the tool instead. "
            "Do not write plans, markdown, literal 'tool_calls:' prose, or assistant_tool_calls_json text when a tool is needed. "
            "Available tools JSON: " + tools_json;
    }
    return
        "Tools are available. When a tool is needed, emit exactly one machine-readable tool call and no explanatory text. "
        "Use <tool_call>{\"name\":\"tool_name\",\"arguments\":{...}}</tool_call> format. "
        "After receiving tool output, either emit the next tool call or provide the final answer. "
        "Do not narrate future tool use such as 'I'll read', 'let me inspect', or 'continue reading'; call the tool instead. "
        "Do not write plans, markdown, literal 'tool_calls:' prose, or assistant_tool_calls_json text when a tool is needed. "
        "Available tools JSON: " + tools_json;
}

std::string RenderQwenToolCalls(const std::string& tool_calls_json) {
    if (tool_calls_json.empty()) return {};
    try {
        auto calls = nlohmann::json::parse(tool_calls_json);
        if (!calls.is_array() || calls.empty()) return {};

        std::string rendered;
        for (const auto& call : calls) {
            std::string name;
            nlohmann::json args;
            if (call.contains("function")) {
                const auto& fn = call["function"];
                name = fn.value("name", "");
                if (fn.contains("arguments")) {
                    const auto& a = fn["arguments"];
                    if (a.is_string()) {
                        try { args = nlohmann::json::parse(a.get<std::string>()); }
                        catch (...) { args = a; }
                    } else {
                        args = a;
                    }
                }
            } else if (call.contains("name")) {
                name = call.value("name", "");
                if (call.contains("arguments")) args = call["arguments"];
            }
            if (name.empty()) continue;

            rendered += "<tool_call>\n<function=" + name + ">\n";
            if (args.is_object()) {
                for (auto it = args.begin(); it != args.end(); ++it) {
                    rendered += "<parameter=" + it.key() + ">\n";
                    std::string val;
                    if (it.value().is_string()) {
                        val = it.value().get<std::string>();
                    } else {
                        val = it.value().dump();
                    }
                    rendered += val + "\n</parameter>\n";
                }
            }
            rendered += "</function>\n</tool_call>";
        }
        return rendered;
    } catch (...) {
        return {};
    }
}

std::string BuildFallbackPrompt(const std::vector<ChatMessage>& messages,
                                const InferenceParams& params,
                                const std::string& model_family) {
    std::string family = model_family.empty() ? "universal" : model_family;
    bool is_qwen = (family == "qwen");
    bool has_tools = !params.tools_json.empty();
    std::ostringstream prompt;
    if (is_qwen && has_tools) {
        auto tool_instruction = BuildToolInstruction(params.tools_json, model_family);
        if (!tool_instruction.empty()) {
            prompt << "<|im_start|>system\n" << tool_instruction << "<|im_end|>\n";
        }
        for (const auto& msg : messages) {
            if (msg.role == MessageRole::Tool) {
                prompt << "<|im_start|>user\n<tool_response>\n" << msg.content << "\n</tool_response><|im_end|>\n";
            } else {
                prompt << "<|im_start|>" << RoleToTemplateRole(msg.role) << "\n";
                if (!msg.name.empty()) prompt << "name: " << msg.name << "\n";
                if (!msg.tool_call_id.empty()) prompt << "tool_call_id: " << msg.tool_call_id << "\n";
                prompt << msg.content;
                if (msg.role == MessageRole::Assistant && !msg.tool_calls_json.empty()) {
                    auto rendered = RenderQwenToolCalls(msg.tool_calls_json);
                    if (!rendered.empty()) prompt << "\n" << rendered;
                }
                prompt << "<|im_end|>\n";
            }
        }
        prompt << "<|im_start|>assistant\n<think>\n\n</think>\n\n";
    } else {
        auto tool_instruction = BuildToolInstruction(params.tools_json, model_family);
        if (!tool_instruction.empty()) {
            prompt << "<|im_start|>system\n" << tool_instruction << "<|im_end|>\n";
        }
        for (const auto& msg : messages) {
            prompt << "<|im_start|>" << RoleToTemplateRole(msg.role) << "\n";
            if (!msg.name.empty()) prompt << "name: " << msg.name << "\n";
            if (!msg.tool_call_id.empty()) prompt << "tool_call_id: " << msg.tool_call_id << "\n";
            prompt << msg.content << "<|im_end|>\n";
        }
        prompt << "<|im_start|>assistant\n";
    }
    return prompt.str();
}

std::string BuildChatPrompt(llama_model* model,
                            const std::vector<ChatMessage>& messages,
                            const InferenceParams& params,
                            const std::string& model_family) {
    std::string family = model_family.empty() ? "universal" : model_family;
    bool is_qwen = (family == "qwen");
    bool has_tools = !params.tools_json.empty();

    if (is_qwen && has_tools) {
        std::vector<std::string> roles;
        std::vector<std::string> contents;

        auto tool_instruction = BuildToolInstruction(params.tools_json, model_family);
        if (!tool_instruction.empty()) {
            roles.push_back("system");
            contents.push_back(tool_instruction);
        }

        for (const auto& msg : messages) {
            if (msg.role == MessageRole::Tool) {
                roles.push_back("user");
                contents.push_back("<tool_response>\n" + msg.content + "\n</tool_response>");
            } else {
                roles.push_back(RoleToTemplateRole(msg.role));
                std::string content;
                if (!msg.name.empty()) content += "name: " + msg.name + "\n";
                if (!msg.tool_call_id.empty()) content += "tool_call_id: " + msg.tool_call_id + "\n";
                content += msg.content;
                if (msg.role == MessageRole::Assistant && !msg.tool_calls_json.empty()) {
                    auto rendered = RenderQwenToolCalls(msg.tool_calls_json);
                    if (!rendered.empty()) content += "\n" + rendered;
                }
                contents.push_back(std::move(content));
            }
        }

        std::vector<llama_chat_message> chat;
        chat.reserve(roles.size());
        for (std::size_t i = 0; i < roles.size(); ++i) {
            chat.push_back({roles[i].c_str(), contents[i].c_str()});
        }

        const char* tmpl = model ? llama_model_chat_template(model, nullptr) : nullptr;
        int32_t needed = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, nullptr, 0);
        if (needed <= 0) return BuildFallbackPrompt(messages, params, model_family);

        std::string prompt(static_cast<std::size_t>(needed), '\0');
        int32_t written = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, prompt.data(), needed);
        if (written < 0) return BuildFallbackPrompt(messages, params, model_family);
        prompt.resize(static_cast<std::size_t>(written));
        return prompt;
    }

    std::vector<std::string> roles;
    std::vector<std::string> contents;
    roles.reserve(messages.size() + 1);
    contents.reserve(messages.size() + 1);

    for (const auto& msg : messages) {
        roles.push_back(RoleToTemplateRole(msg.role));
        std::string content;
        if (!msg.name.empty()) content += "name: " + msg.name + "\n";
        if (!msg.tool_call_id.empty()) content += "tool_call_id: " + msg.tool_call_id + "\n";
        content += msg.content;
        contents.push_back(std::move(content));
    }

    auto tool_instruction = BuildToolInstruction(params.tools_json, model_family);
    if (!tool_instruction.empty()) {
        roles.insert(roles.begin(), "system");
        contents.insert(contents.begin(), std::move(tool_instruction));
    }

    std::vector<llama_chat_message> chat;
    chat.reserve(roles.size());
    for (std::size_t i = 0; i < roles.size(); ++i) {
        chat.push_back({roles[i].c_str(), contents[i].c_str()});
    }

    const char* tmpl = model ? llama_model_chat_template(model, nullptr) : nullptr;
    int32_t needed = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, nullptr, 0);
    if (needed <= 0) return BuildFallbackPrompt(messages, params, model_family);

    std::string prompt(static_cast<std::size_t>(needed), '\0');
    int32_t written = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, prompt.data(), needed);
    if (written < 0) return BuildFallbackPrompt(messages, params, model_family);
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

llama_sampler* CreateSampler(const InferenceParams& params) {
    auto sparams = llama_sampler_chain_default_params();
    sparams.no_perf = true;
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

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = static_cast<uint32_t>(std::max(512, params.context_size));
    ctx_params.n_batch = static_cast<uint32_t>(params.batch_size);
    ctx_params.n_ubatch = static_cast<uint32_t>(params.batch_size);
    ctx_params.n_seq_max = 1;
    ctx_params.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    ctx_params.offload_kqv = true;
    ctx_params.kv_unified = true;
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

    if (!params.mmproj_path.empty()) {
        auto mmproj_params = mtmd_context_params_default();
        mmproj_params.use_gpu = true;
        mmproj_ctx_ = mtmd_init_from_file(params.mmproj_path.c_str(), static_cast<llama_model*>(model), mmproj_params);
        if (mmproj_ctx_) {
            has_vision_ = true;
            Logger::Get().Info("Vision model initialized with mmproj: " + params.mmproj_path);
        } else {
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

    bool reuse_cache = cache_valid_ &&
                       messages.size() > last_message_count_ &&
                       last_message_count_ > 0;

    std::string model_family;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        model_family = DetectModelFamily(current_model_name_);
    }
    if (!params.tool_format_override.empty()) {
        model_family = params.tool_format_override;
    }
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
        if (reuse_cache) {
            llama_memory_clear(llama_get_memory(context), true);
            n_past = 0;
            reuse_cache = false;
        }
        std::string prompt = BuildChatPrompt(model, messages, params, model_family);
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
            prompt = BuildChatPrompt(model, messages, params, model_family);
            prompt_tokens = Tokenize(model, prompt, true);
        }
    } else {
        std::string prompt = BuildChatPrompt(model, messages, params, model_family);
        const std::string think_suffix = "<|im_start|>assistant\n<think>\n";
        const std::string no_think_suffix = "<|im_start|>assistant\n<think>\n\n</think>\n\n";
        if (prompt.size() >= think_suffix.size() &&
            prompt.compare(prompt.size() - think_suffix.size(), think_suffix.size(), think_suffix) == 0) {
            prompt.replace(prompt.size() - think_suffix.size(), think_suffix.size(), no_think_suffix);
        }
        prompt_tokens = Tokenize(model, prompt, true);
    }
    if (prompt_tokens.empty() && !multimodal_used) {
        result.http_status = 500;
        result.error_message = "Failed to tokenize prompt";
        return result;
    }

    const int32_t model_max_ctx = llama_model_n_ctx_train(model);
    const int max_predict = params.max_tokens > 0 ? params.max_tokens : model_max_ctx;
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

    llama_sampler* sampler = CreateSampler(params);
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
