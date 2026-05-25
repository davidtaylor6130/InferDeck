#include "routes/ChatCompletions.hpp"
#include "RuntimeActivity.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "config/ConfigLoader.hpp"
#include "core/Config.hpp"
#include "core/Logger.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <chrono>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <memory>
#include <vector>
#include <unordered_map>
#include <string_view>
#include <thread>
#include <cstdint>
#include <cstdlib>
using json = nlohmann::json;

namespace {

static std::string MakeModelId(const std::string& full_path) {
    if (full_path.empty()) return "local-model";
    std::filesystem::path p(full_path);
    std::string name = p.stem().string();
    for (auto& c : name) {
        if (c == ' ' || c == '_' || c == '-') c = '-';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

static const std::vector<std::string> kQuantSuffixes = {
    "-q4_k_m", "-q4_k_s", "-q4_0", "-q5_k_m", "-q5_k_s", "-q5_0",
    "-q6_k", "-q6_k_m", "-q8_0", "-q2_k", "-q3_k_m", "-q3_k_s",
    "-iq1_s", "-iq2_s", "-iq2_xs", "-iq2_xxs", "-iq3_s", "-iq3_xs",
    "-iq3_xxs", "-iq4_nl", "-iq4_xs", "-iq4_xxs",
    "-iq1_m", "-iq2_m", "-iq3_m",
    "-mxfp4", "-mx4", "-mx6", "-mx8",
    "-ud-iq3_xxs", "-ud-iq2_xs",
    "-f16", "-f32", "-bf16",
    "-gguf",
    ".q4_k_m", ".q4_k_s", ".q4_0", ".q5_k_m", ".q5_k_s", ".q5_0",
    ".q6_k", ".q6_k_m", ".q8_0", ".q2_k", ".q3_k_m", ".q3_k_s",
    ".iq1_s", ".iq2_s", ".iq2_xs", ".iq2_xxs", ".iq3_s", ".iq3_xs",
    ".iq3_xxs", ".iq4_nl", ".iq4_xs", ".iq4_xxs",
    ".iq1_m", ".iq2_m", ".iq3_m",
    ".mxfp4", ".mx4", ".mx6", ".mx8",
    ".ud-iq3_xxs", ".ud-iq2_xs",
    ".f16", ".f32", ".bf16",
    ".gguf"
};

static std::string NormalizeModelName(const std::string& name) {
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string clean = name;
    for (const auto& suffix : kQuantSuffixes) {
        size_t pos = lower.rfind(suffix);
        if (pos != std::string::npos && pos + suffix.size() == lower.size()) {
            clean = name.substr(0, pos);
            break;
        }
    }

    for (auto& c : clean) {
        if (c == ' ' || c == '_' || c == '-') c = '-';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return clean;
}

static std::string MakeCleanModelId(const std::string& full_path) {
    if (full_path.empty()) return "local-model";
    std::filesystem::path p(full_path);
    std::string name = p.stem().string();
    return NormalizeModelName(name);
}

static std::string NormalizeId(const std::string& id) {
    std::string result = id;
    constexpr size_t latest_suffix_len = 7;
    if (result.size() > latest_suffix_len && result.compare(result.size() - latest_suffix_len, latest_suffix_len, ":latest") == 0) {
        result = result.substr(0, result.size() - latest_suffix_len);
    }
    for (auto& c : result) {
        if (c == ' ' || c == '_' || c == ':') c = '-';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

struct ModelEntry {
    std::string path;
    std::string clean_id;
    std::string full_id;
};

static std::vector<ModelEntry> g_model_cache;
static std::mutex g_model_cache_mutex;
static bool g_cache_populated = false;
static std::mutex g_switch_mutex;

static void PopulateModelCache(const std::string& model_dir) {
    if (g_cache_populated) return;

    std::vector<std::string> extensions = {".gguf", ".bin", ".ggml"};
    std::unordered_map<std::string, ModelEntry> clean_map;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(model_dir)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            std::string filename = entry.path().filename().string();
            if (filename.find("mmproj") == 0) continue;
            for (const auto& e : extensions) {
                if (ext == e) {
                    ModelEntry me;
                    me.path = entry.path().string();
                    me.clean_id = MakeCleanModelId(me.path);
                    me.full_id = MakeModelId(me.path);

                    if (clean_map.find(me.clean_id) == clean_map.end()) {
                        clean_map[me.clean_id] = me;
                    }
                    g_model_cache.push_back(me);
                    break;
                }
            }
        }
    }

    g_cache_populated = true;
}

static void RefreshModelCache(const std::string& model_dir) {
    g_model_cache.clear();
    g_cache_populated = false;
    PopulateModelCache(model_dir);
}

static std::string FindModelPath(const std::string& model_id) {
    std::lock_guard<std::mutex> lock(g_model_cache_mutex);

    std::string normalized = NormalizeId(model_id);
    if (normalized.empty()) return "";

    std::string model_dir;
    try {
        auto full = inferdeck::core::Config::Load(inferdeck::gateway::GetDefaultConfigPath());
        model_dir = full.model.directory;
    } catch (...) { return ""; }

    if (model_dir.empty() || !std::filesystem::exists(model_dir)) return "";

    if (normalized == "gpt-oss-20b" || normalized == "openai-gpt-oss-20b") {
        auto path = std::filesystem::path(model_dir) / "openai_gpt-oss-20b-GGUF" / "openai_gpt-oss-20b-MXFP4.gguf";
        if (std::filesystem::exists(path)) return path.string();
    }

    PopulateModelCache(model_dir);

    for (const auto& entry : g_model_cache) {
        if (entry.clean_id == normalized || entry.full_id == normalized) {
            return entry.path;
        }
    }

    for (const auto& entry : g_model_cache) {
        if (entry.clean_id.find(normalized) != std::string::npos ||
            normalized.find(entry.clean_id) != std::string::npos) {
            return entry.path;
        }
    }

    for (const auto& entry : g_model_cache) {
        std::string short_clean = entry.clean_id;
        size_t dash = short_clean.find_last_of('-');
        if (dash != std::string::npos) {
            std::string prefix = short_clean.substr(0, dash);
            if (normalized.find(prefix) != std::string::npos || prefix.find(normalized) != std::string::npos) {
                return entry.path;
            }
        }
    }

    RefreshModelCache(model_dir);

    for (const auto& entry : g_model_cache) {
        if (entry.clean_id == normalized || entry.full_id == normalized ||
            entry.clean_id.find(normalized) != std::string::npos ||
            normalized.find(entry.clean_id) != std::string::npos) {
            return entry.path;
        }
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(model_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = NormalizeId(entry.path().stem().string());
        if (entry.path().extension() == ".gguf" &&
            (filename.find(normalized) != std::string::npos || normalized.find(filename) != std::string::npos)) {
            return entry.path().string();
        }
    }

    return "";
}

static std::string GetCurrentCleanModelId() {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    std::string model_name = engine.GetModelName();
    if (model_name.empty()) return "";
    return NormalizeModelName(model_name);
}

}

namespace inferdeck::gateway::routes {

static std::string MakeId() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return "chatcmpl-" + std::to_string(ms);
}

static std::string SseDeltaChunk(const std::string& id, const std::string& model, const json& delta, const json& finish_reason = nullptr);
static std::string SseErrorChunk(const std::string& id, const std::string& model, const std::string& message);

static std::size_t CountSseDataChunks(const std::string& stream) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = stream.find("data: ", pos)) != std::string::npos) {
        ++count;
        pos += 6;
    }
    return count;
}

static std::string ResponseFinishReason(const nlohmann::json& response) {
    if (!response.contains("choices") || !response["choices"].is_array() || response["choices"].empty()) return "stop";
    const auto& choice = response["choices"][0];
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
        return choice["finish_reason"].get<std::string>();
    }
    return "stop";
}

static std::size_t ResponseToolCallCount(const nlohmann::json& response) {
    if (!response.contains("choices") || !response["choices"].is_array() || response["choices"].empty()) return 0;
    const auto& choice = response["choices"][0];
    if (!choice.contains("message") || !choice["message"].is_object()) return 0;
    const auto& message = choice["message"];
    if (!message.contains("tool_calls") || !message["tool_calls"].is_array()) return 0;
    return message["tool_calls"].size();
}

static std::string TrimCopy(std::string text);

static std::string JsonArgumentString(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_object() || value.is_array()) return value.dump();
    if (value.is_null()) return "{}";
    return value.dump();
}

static std::string MakeToolCallId(std::size_t index) {
    return "call_inferdeck_" + std::to_string(index + 1);
}

nlohmann::json NormalizeOpenAiToolCalls(const nlohmann::json& tool_calls) {
    nlohmann::json normalized = nlohmann::json::array();
    if (!tool_calls.is_array()) return normalized;

    std::size_t index = 0;
    for (const auto& raw : tool_calls) {
        if (!raw.is_object()) continue;
        nlohmann::json item;
        item["id"] = raw.value("id", MakeToolCallId(index));
        item["type"] = raw.value("type", "function");
        if (!raw.contains("function") || !raw["function"].is_object()) continue;
        const auto& function = raw["function"];
        const auto name = function.value("name", "");
        if (name.empty()) continue;
        item["function"] = {
            {"name", name},
            {"arguments", function.contains("arguments") ? JsonArgumentString(function["arguments"]) : "{}"}
        };
        normalized.push_back(std::move(item));
        ++index;
    }
    return normalized;
}

static nlohmann::json ExtractQwenToolCallsFromText(const std::string& text) {
    nlohmann::json calls = nlohmann::json::array();
    std::size_t search = 0;
    while (true) {
        const auto start = text.find("<tool_call>", search);
        if (start == std::string::npos) break;
        const auto body_start = start + std::string("<tool_call>").size();
        const auto end = text.find("</tool_call>", body_start);
        if (end == std::string::npos) break;
        auto payload = TrimCopy(text.substr(body_start, end - body_start));
        search = end + std::string("</tool_call>").size();
        if (payload.empty()) continue;
        try {
            auto parsed = nlohmann::json::parse(payload);
            nlohmann::json raw_call;
            raw_call["id"] = MakeToolCallId(calls.size());
            raw_call["type"] = "function";
            if (parsed.contains("function") && parsed["function"].is_object()) {
                raw_call["function"] = parsed["function"];
            } else {
                const auto name = parsed.value("name", parsed.value("function", ""));
                if (name.empty()) continue;
                raw_call["function"] = {
                    {"name", name},
                    {"arguments", parsed.contains("arguments") ? parsed["arguments"] : nlohmann::json::object()}
                };
            }
            auto normalized = NormalizeOpenAiToolCalls(nlohmann::json::array({raw_call}));
            if (!normalized.empty()) calls.push_back(normalized.front());
        } catch (...) {
            continue;
        }
    }
    return calls;
}

static std::string RemoveQwenToolCallBlocks(std::string text) {
    while (true) {
        const auto start = text.find("<tool_call>");
        if (start == std::string::npos) break;
        const auto end = text.find("</tool_call>", start);
        if (end == std::string::npos) {
            text.erase(start);
            break;
        }
        text.erase(start, end + std::string("</tool_call>").size() - start);
    }
    return TrimCopy(text);
}

std::string ValidateChatRequest(const std::string& body) {
    if (body.empty()) return "Request body is required";

    try {
        auto parsed = json::parse(body);
        if (!parsed.contains("messages") || !parsed["messages"].is_array()) {
            return "Missing or invalid messages field";
        }

        for (const auto& message : parsed["messages"]) {
            if (!message.contains("role") || !message["role"].is_string()) {
                return "Each message requires a role";
            }
            std::string role = message["role"].get<std::string>();
            if (role != "system" && role != "user" && role != "assistant" && role != "tool") {
                return "Invalid message role";
            }
        }
    } catch (const std::exception& e) {
        return e.what();
    }

    return "";
}

bool ShouldForceNonStreamingBackend(const nlohmann::json& request) {
    return request.contains("tools") && request["tools"].is_array() && !request["tools"].empty();
}

static std::string SafeLogText(std::string text, std::size_t limit = 120) {
    for (auto& c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc == 127) c = ' ';
    }
    if (text.size() > limit) text.resize(limit);
    return text;
}

bool ShouldUseStreamingBackend(const nlohmann::json& request) {
    return request.value("stream", false) && !ShouldForceNonStreamingBackend(request);
}

bool ShouldUseStreamingBackendForClient(const nlohmann::json& request, const std::string& client) {
    (void)client;
    return request.value("stream", false) &&
           !ShouldForceNonStreamingBackend(request);
}

static void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = text.find(from.data(), pos, from.size())) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string TrimCopy(std::string text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

static void AppendReasoning(std::string& reasoning, const std::string& text) {
    auto clean = TrimCopy(text);
    if (clean.empty()) return;
    if (!reasoning.empty()) reasoning += "\n";
    reasoning += clean;
}

std::string ExtractAssistantReasoningContent(const std::string& content) {
    std::string text = content;
    std::string reasoning;

    while (true) {
        const auto start = text.find("<think>");
        if (start == std::string::npos) break;
        const auto content_start = start + std::string("<think>").size();
        const auto end = text.find("</think>", content_start);
        if (end == std::string::npos) {
            AppendReasoning(reasoning, text.substr(content_start));
            break;
        }
        AppendReasoning(reasoning, text.substr(content_start, end - content_start));
        text.erase(start, end + std::string("</think>").size() - start);
    }

    while (true) {
        const auto start = text.find("<|channel|>analysis<|message|>");
        if (start == std::string::npos) break;
        const auto content_start = start + std::string("<|channel|>analysis<|message|>").size();
        const auto end = text.find("<|end|>", content_start);
        if (end == std::string::npos) {
            AppendReasoning(reasoning, text.substr(content_start));
            break;
        }
        AppendReasoning(reasoning, text.substr(content_start, end - content_start));
        text.erase(start, end + std::string("<|end|>").size() - start);
    }

    return reasoning;
}

std::string SanitizeAssistantContent(const std::string& content) {
    std::string text = content;

    while (true) {
        const auto start = text.find("<think>");
        if (start == std::string::npos) break;
        const auto end = text.find("</think>", start);
        if (end == std::string::npos) {
            text.erase(start);
            break;
        }
        text.erase(start, end + std::string("</think>").size() - start);
    }

    while (true) {
        const auto channel = text.find("<|channel|>analysis<|message|>");
        if (channel == std::string::npos) break;
        const auto end = text.find("<|end|>", channel);
        if (end == std::string::npos) {
            text.erase(channel);
            break;
        }
        text.erase(channel, end + std::string("<|end|>").size() - channel);
    }

    ReplaceAll(text, "<|start|>assistant", "");
    ReplaceAll(text, "<|start|>system", "");
    ReplaceAll(text, "<|start|>user", "");
    ReplaceAll(text, "<|channel|>final<|message|>", "");
    ReplaceAll(text, "<|channel|>commentary<|message|>", "");
    ReplaceAll(text, "<|channel|>analysis", "");
    ReplaceAll(text, "<|channel|>final", "");
    ReplaceAll(text, "<|channel|>commentary", "");
    ReplaceAll(text, "<|message|>", "");
    ReplaceAll(text, "<|start|>", "");
    ReplaceAll(text, "<|channel|>", "");
    ReplaceAll(text, "<|end|>", "");
    return TrimCopy(text);
}

std::string BuildSyntheticChatCompletionStream(const nlohmann::json& response) {
    std::string id = response.value("id", MakeId());
    std::string model = response.value("model", "local-model");
    std::string stream;

    stream += SseDeltaChunk(id, model, json({{"role", "assistant"}}));

    std::string finish_reason = "stop";
    if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty()) {
        const auto& choice = response["choices"][0];
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
            finish_reason = choice["finish_reason"].get<std::string>();
        }

        if (choice.contains("message") && choice["message"].is_object()) {
            const auto& message = choice["message"];
            std::string reasoning_content;
            json extracted_tool_calls = json::array();
            if (message.contains("reasoning_content") && message["reasoning_content"].is_string()) {
                AppendReasoning(reasoning_content, message["reasoning_content"].get<std::string>());
            }
            if (message.contains("content") && message["content"].is_string() && !message["content"].get<std::string>().empty()) {
                const auto raw_content = message["content"].get<std::string>();
                extracted_tool_calls = ExtractQwenToolCallsFromText(raw_content);
                const auto without_tool_calls = RemoveQwenToolCallBlocks(raw_content);
                AppendReasoning(reasoning_content, ExtractAssistantReasoningContent(without_tool_calls));
                const auto clean_content = SanitizeAssistantContent(without_tool_calls);
                if (!reasoning_content.empty()) {
                    stream += SseDeltaChunk(id, model, json({{"reasoning_content", reasoning_content}}));
                }
                if (!clean_content.empty()) {
                    stream += SseDeltaChunk(id, model, json({{"content", clean_content}}));
                }
            } else if (!reasoning_content.empty()) {
                stream += SseDeltaChunk(id, model, json({{"reasoning_content", reasoning_content}}));
            }
            auto tool_calls = NormalizeOpenAiToolCalls(message.contains("tool_calls") ? message["tool_calls"] : json::array());
            for (const auto& tool_call : extracted_tool_calls) tool_calls.push_back(tool_call);
            if (!tool_calls.empty()) {
                for (size_t i = 0; i < tool_calls.size(); ++i) {
                    json tool_call = tool_calls[i];
                    tool_call["index"] = i;
                    stream += SseDeltaChunk(id, model, json({{"tool_calls", json::array({tool_call})}}));
                }
            }
        }
    }

    stream += SseDeltaChunk(id, model, json::object(), finish_reason);
    stream += "data: [DONE]\n\n";
    return stream;
}

static std::string SseChunk(const std::string& id, const std::string& model, const std::string& content, const std::string& reasoning_content, bool done) {
    json chunk;
    chunk["id"] = id;
    chunk["object"] = "chat.completion.chunk";
    chunk["model"] = model;
    chunk["created"] = std::time(nullptr);
    if (done) {
        chunk["choices"] = json::array({{{"index", 0}, {"delta", json::object()}, {"finish_reason", "stop"}}});
    } else {
        json delta = {{"role", "assistant"}};
        if (!content.empty()) {
            delta["content"] = content;
        }
        if (!reasoning_content.empty()) {
            delta["reasoning_content"] = reasoning_content;
        }
        chunk["choices"] = json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", nullptr}}});
    }
    return "data: " + chunk.dump() + "\n\n";
}

static std::string SseDeltaChunk(const std::string& id, const std::string& model, const json& delta, const json& finish_reason) {
    json chunk;
    chunk["id"] = id;
    chunk["object"] = "chat.completion.chunk";
    chunk["model"] = model;
    chunk["created"] = std::time(nullptr);
    chunk["choices"] = json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish_reason}}});
    return "data: " + chunk.dump() + "\n\n";
}

static std::string SseErrorChunk(const std::string& id, const std::string& model, const std::string& message) {
    json chunk;
    chunk["id"] = id;
    chunk["object"] = "chat.completion.chunk";
    chunk["model"] = model;
    chunk["created"] = std::time(nullptr);
    chunk["choices"] = json::array({{
        {"index", 0},
        {"delta", json::object()},
        {"finish_reason", "error"}
    }});
    chunk["error"] = {{"message", message}, {"type", "backend_error"}, {"code", "backend_stream_error"}};
    return "data: " + chunk.dump() + "\n\n";
}

static json BuildChatCompletionResponse(const std::string& id,
                                        const std::string& model,
                                        const inferdeck::core::InferenceResult& result) {
    json response;
    response["id"] = id;
    response["object"] = "chat.completion";
    response["created"] = std::time(nullptr);
    response["model"] = model;

    json message = {{"role", "assistant"}};
    auto qwen_tool_calls = ExtractQwenToolCallsFromText(result.text);
    const auto visible_text = RemoveQwenToolCallBlocks(result.text);
    if (!visible_text.empty()) message["content"] = SanitizeAssistantContent(visible_text);
    std::string reasoning_text = result.reasoning_text;
    AppendReasoning(reasoning_text, ExtractAssistantReasoningContent(visible_text));
    if (!reasoning_text.empty()) {
        message["reasoning_content"] = reasoning_text;
    }
    json tool_calls = json::array();
    if (!result.tool_calls.empty()) {
        for (const auto& tc : result.tool_calls) {
            json tc_json;
            tc_json["id"] = tc.id;
            tc_json["type"] = tc.type;
            tc_json["function"] = {{"name", tc.function_name}, {"arguments", JsonArgumentString(tc.function_arguments)}};
            tool_calls.push_back(tc_json);
        }
    }
    for (const auto& tc : qwen_tool_calls) tool_calls.push_back(tc);
    tool_calls = NormalizeOpenAiToolCalls(tool_calls);
    if (!tool_calls.empty()) message["tool_calls"] = tool_calls;
    if (!message.contains("content") && tool_calls.empty()) message["content"] = "";

    std::string finish_reason = tool_calls.empty() ? "stop" : "tool_calls";
    response["choices"] = json::array({{{"index", 0}, {"message", message}, {"finish_reason", finish_reason}}});
    response["usage"] = {{"prompt_tokens", result.prompt_tokens}, {"completion_tokens", result.completion_tokens}, {"total_tokens", result.total_tokens}};
    return response;
}

std::string DetectChatClientName(const httplib::Request& req) {
    std::string explicit_client = req.get_header_value("X-InferDeck-Client");
    if (!explicit_client.empty()) return explicit_client.substr(0, 80);
    std::string user_agent = req.get_header_value("User-Agent");
    std::string lower = user_agent;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("open-webui") != std::string::npos) return "Open WebUI";
    if (lower.find("opencode") != std::string::npos) return "OpenCode";
    if (lower.find("ollama") != std::string::npos) return "Ollama compatible";
    if (!user_agent.empty()) return user_agent.substr(0, 80);
    return "OpenAI compatible";
}

static json BuildJobPayloadPreview(const json& body, bool stream, bool force_non_streaming_backend, const std::string& request_protocol) {
    json preview;
    preview["requestProtocol"] = request_protocol;
    preview["model"] = body.value("model", "");
    preview["stream"] = stream;
    preview["tools"] = body.contains("tools") && body["tools"].is_array() ? body["tools"].size() : 0;
    preview["messages"] = body.contains("messages") && body["messages"].is_array() ? body["messages"].size() : 0;
    preview["forcedNonStreamingBackend"] = force_non_streaming_backend;
    if (body.contains("max_tokens")) preview["maxTokens"] = body["max_tokens"];
    if (body.contains("temperature")) preview["temperature"] = body["temperature"];
    return preview;
}

static void SetInferenceError(httplib::Response& resp, const inferdeck::core::InferenceResult& result) {
    int status = result.http_status >= 400 ? result.http_status : 502;
    resp.status = status;
    resp.set_content(json({{"error", {{"message", result.error_message.empty() ? "Backend inference failed" : result.error_message},
                                      {"type", "backend_error"},
                                      {"code", status}}}}).dump(), "application/json");
}

struct StreamQueue {
    std::queue<std::string> chunks;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> done{false};

    void push(std::string chunk) {
        std::lock_guard<std::mutex> lock(mtx);
        chunks.push(std::move(chunk));
        cv.notify_one();
    }

    bool pop(std::string& out) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return !chunks.empty() || done.load(); });
        if (chunks.empty()) return false;
        out = std::move(chunks.front());
        chunks.pop();
        return true;
    }

    bool pop_for(std::string& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, timeout, [this] { return !chunks.empty() || done.load(); });
        if (chunks.empty()) return false;
        out = std::move(chunks.front());
        chunks.pop();
        return true;
    }

    void finish() {
        done.store(true);
        cv.notify_all();
    }
};

struct StreamState {
    std::shared_ptr<StreamQueue> queue = std::make_shared<StreamQueue>();
    std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::atomic<bool> abandoned{false};
    std::atomic<bool> abort_requested{false};
    std::mutex mtx;

    void touch() {
        std::lock_guard<std::mutex> lock(mtx);
        last_activity = std::chrono::steady_clock::now();
    }

    std::chrono::steady_clock::time_point last() {
        std::lock_guard<std::mutex> lock(mtx);
        return last_activity;
    }

    bool request_abort() {
        bool expected = false;
        return abort_requested.compare_exchange_strong(expected, true);
    }
};

static int EnvIntMs(const char* name, int fallback_ms) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback_ms;
    try {
        int parsed = std::stoi(value);
        return parsed > 0 ? parsed : fallback_ms;
    } catch (...) {
        return fallback_ms;
    }
}

struct GuardedPredictState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    inferdeck::core::InferenceResult result;
    std::string exception;
};

static nlohmann::json SyntheticResultPreview(const nlohmann::json& response,
                                             const inferdeck::core::InferenceResult& result,
                                             const std::string& request_protocol,
                                             const std::string& response_mode,
                                             std::uint64_t sse_chunks,
                                             std::uint64_t heartbeat_chunks,
                                             std::uint64_t response_bytes,
                                             const std::string& cause = "") {
    nlohmann::json preview = {
        {"requestProtocol", request_protocol},
        {"responseProtocol", "openai.sse"},
        {"responseMode", response_mode},
        {"sseChunks", sse_chunks},
        {"heartbeatChunks", heartbeat_chunks},
        {"responseBytes", response_bytes},
        {"finishReason", ResponseFinishReason(response)},
        {"toolCallCount", ResponseToolCallCount(response)},
        {"contentPreview", result.text.substr(0, 500)},
        {"reasoningPreview", result.reasoning_text.substr(0, 500)},
        {"waitingOnBackendOrToolFormatting", false},
        {"backendAbortState", "none"},
        {"syntheticStream", true}
    };
    if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty() &&
        response["choices"][0].contains("message")) {
        const auto& message = response["choices"][0]["message"];
        preview["toolCalls"] = message.contains("tool_calls") ? message["tool_calls"] : nlohmann::json::array();
    } else {
        preview["toolCalls"] = nlohmann::json::array();
    }
    if (response.contains("usage")) preview["usage"] = response["usage"];
    if (!cause.empty()) preview["terminalCause"] = cause;
    return preview;
}

static bool RunGuardedSyntheticSse(inferdeck::core::LlamaEngine& engine,
                                   const std::vector<inferdeck::core::ChatMessage>& messages,
                                   const inferdeck::core::InferenceParams& params,
                                   const std::string& id,
                                   const std::string& response_model_id,
                                   const std::string& job_id,
                                   const std::string& log_label,
                                   const std::string& request_protocol,
                                   httplib::DataSink& sink) {
    constexpr int kDefaultHeartbeatMs = 5000;
    constexpr int kDefaultIdleTimeoutMs = 60000;
    constexpr int kDefaultTotalTimeoutMs = 600000;

    const auto heartbeat_interval = std::chrono::milliseconds(EnvIntMs("INFERDECK_OPENCODE_HEARTBEAT_MS", kDefaultHeartbeatMs));
    const auto idle_timeout = std::chrono::milliseconds(EnvIntMs("INFERDECK_OPENCODE_IDLE_TIMEOUT_MS", kDefaultIdleTimeoutMs));
    const auto total_timeout = std::chrono::milliseconds(EnvIntMs("INFERDECK_OPENCODE_TOTAL_TIMEOUT_MS", kDefaultTotalTimeoutMs));

    auto state = std::make_shared<GuardedPredictState>();
    auto terminal_sent = std::make_shared<std::atomic<bool>>(false);
    std::uint64_t sse_chunks_sent = 0;
    std::uint64_t sse_bytes_sent = 0;
    std::uint64_t heartbeat_chunks_sent = 0;

    auto mark_failed_once = [&](const std::string& cause, int status_code) {
        bool expected = false;
        if (!terminal_sent->compare_exchange_strong(expected, true)) return;
        inferdeck::gateway::RuntimeActivity::Get().FailJob(job_id, cause, status_code);
    };

    auto write_chunk = [&](const std::string& chunk) {
        sse_chunks_sent += CountSseDataChunks(chunk);
        sse_bytes_sent += chunk.size();
        return sink.write(chunk.data(), chunk.size());
    };

    std::thread([state, &engine, messages, params]() {
        inferdeck::core::InferenceResult result;
        std::string exception;
        try {
            std::unique_lock<std::mutex> backend_lock(g_switch_mutex);
            result = engine.Predict(messages, params);
        } catch (const std::exception& e) {
            exception = e.what();
        } catch (...) {
            exception = "Unknown backend inference failure";
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->result = std::move(result);
            state->exception = std::move(exception);
            state->done = true;
        }
        state->cv.notify_all();
    }).detach();

    const auto start = std::chrono::steady_clock::now();
    auto last_backend_activity = start;
    auto next_heartbeat = start;

    inferdeck::core::Logger::Get().Info("Request " + job_id + " using guarded synthetic SSE for " + log_label);

    while (true) {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->cv.wait_until(lock, next_heartbeat, [&] { return state->done; })) {
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - start >= total_timeout) {
            const std::string cause = "OpenCode " + log_label + " totalTimeout";
            inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, nlohmann::json({
                {"requestProtocol", request_protocol},
                {"responseProtocol", "openai.sse"},
                {"responseMode", "guarded-synthetic-sse-error"},
                {"finishReason", "error"},
                {"terminalCause", "timeout"},
                {"backendAbortState", "aborted"},
                {"waitingOnBackendOrToolFormatting", false}
            }), "Guarded synthetic SSE timed out");
            mark_failed_once(cause, 504);
            inferdeck::gateway::RuntimeActivity::Get().FailRunningClientJobs("OpenCode", "backendAborted: " + cause, 504, job_id);
            inferdeck::core::LlamaEngine::Get().AbortActiveRequest(cause);
            const auto error_stream = SseErrorChunk(id, response_model_id, cause) +
                                      SseDeltaChunk(id, response_model_id, nlohmann::json::object(), "error") +
                                      "data: [DONE]\n\n";
            write_chunk(error_stream);
            sink.done();
            return false;
        }

        if (now - last_backend_activity >= idle_timeout) {
            const std::string cause = "OpenCode " + log_label + " idleTimeout";
            inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, nlohmann::json({
                {"requestProtocol", request_protocol},
                {"responseProtocol", "openai.sse"},
                {"responseMode", "guarded-synthetic-sse-error"},
                {"finishReason", "error"},
                {"terminalCause", "idle-timeout"},
                {"backendAbortState", "aborted"},
                {"waitingOnBackendOrToolFormatting", false}
            }), "Guarded synthetic SSE idle timed out");
            mark_failed_once(cause, 504);
            inferdeck::gateway::RuntimeActivity::Get().FailRunningClientJobs("OpenCode", "backendAborted: " + cause, 504, job_id);
            inferdeck::core::LlamaEngine::Get().AbortActiveRequest(cause);
            const auto error_stream = SseErrorChunk(id, response_model_id, cause) +
                                      SseDeltaChunk(id, response_model_id, nlohmann::json::object(), "error") +
                                      "data: [DONE]\n\n";
            write_chunk(error_stream);
            sink.done();
            return false;
        }

        const std::string heartbeat = ": inferdeck opencode " + log_label + " backend running\n\n";
        if (!write_chunk(heartbeat)) {
            const std::string cause = "OpenCode " + log_label + " clientDisconnected";
            inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, nlohmann::json({
                {"requestProtocol", request_protocol},
                {"responseProtocol", "openai.sse"},
                {"responseMode", "guarded-synthetic-sse-error"},
                {"finishReason", "error"},
                {"terminalCause", "client-disconnect"},
                {"backendAbortState", "aborted"},
                {"waitingOnBackendOrToolFormatting", false}
            }), "Guarded synthetic SSE client disconnected");
            mark_failed_once(cause, 499);
            inferdeck::gateway::RuntimeActivity::Get().FailRunningClientJobs("OpenCode", "backendAborted: " + cause, 499, job_id);
            inferdeck::core::LlamaEngine::Get().AbortActiveRequest(cause);
            sink.done();
            return false;
        }
        ++heartbeat_chunks_sent;
        next_heartbeat = now + heartbeat_interval;
    }

    inferdeck::core::InferenceResult result;
    std::string exception;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        result = state->result;
        exception = state->exception;
    }

    if (!exception.empty()) {
        const auto error_stream = SseErrorChunk(id, response_model_id, exception) +
                                  SseDeltaChunk(id, response_model_id, nlohmann::json::object(), "error") +
                                  "data: [DONE]\n\n";
        inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, nlohmann::json({
            {"requestProtocol", request_protocol},
            {"responseProtocol", "openai.sse"},
            {"responseMode", "guarded-synthetic-sse-error"},
            {"finishReason", "error"},
            {"terminalCause", "backend-exception"},
            {"backendAbortState", "exception"},
            {"waitingOnBackendOrToolFormatting", false}
        }), "Guarded synthetic SSE backend exception");
        mark_failed_once(exception, 500);
        write_chunk(error_stream);
        sink.done();
        return false;
    }

    if (result.HasError()) {
        const auto error_stream = SseErrorChunk(id, response_model_id, result.error_message) +
                                  SseDeltaChunk(id, response_model_id, nlohmann::json::object(), "error") +
                                  "data: [DONE]\n\n";
        bool expected = false;
        if (terminal_sent->compare_exchange_strong(expected, true)) {
            inferdeck::gateway::RuntimeActivity::Get().CompleteJob(job_id, result, nlohmann::json({
                {"responseMode", "guarded-synthetic-sse-error"},
                {"requestProtocol", request_protocol},
                {"responseProtocol", "openai.sse"},
                {"sseChunks", CountSseDataChunks(error_stream)},
                {"heartbeatChunks", heartbeat_chunks_sent},
                {"responseBytes", error_stream.size()},
                {"finishReason", "error"},
                {"toolCallCount", 0},
                {"error", result.error_message},
                {"terminalCause", "backendError"},
                {"backendAbortState", "backend-error"},
                {"waitingOnBackendOrToolFormatting", false},
                {"syntheticStream", true}
            }));
        }
        write_chunk(error_stream);
        sink.done();
        return false;
    }

    nlohmann::json response = BuildChatCompletionResponse(id, response_model_id, result);
    const auto synthetic_stream = BuildSyntheticChatCompletionStream(response);
    bool expected = false;
    if (terminal_sent->compare_exchange_strong(expected, true)) {
        inferdeck::gateway::RuntimeActivity::Get().CompleteJob(
            job_id,
            result,
            SyntheticResultPreview(response,
                                   result,
                                   request_protocol,
                                   "guarded-synthetic-sse",
                                   CountSseDataChunks(synthetic_stream),
                                   heartbeat_chunks_sent,
                                   synthetic_stream.size()));
    }
    write_chunk(synthetic_stream);
    sink.done();
    return false;
}

void HandleChatCompletions(const httplib::Request& req, httplib::Response& resp) {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    if (!engine.IsInitialized()) {
        resp.status = 503;
        resp.set_content(R"({"error":{"message":"Engine not initialized","type":"service_error","code":503}})", "application/json");
        return;
    }
    try {
        auto body = json::parse(req.body);
        bool client_requested_stream = body.value("stream", false);
        bool stream = client_requested_stream;

        std::vector<inferdeck::core::ChatMessage> messages;
        if (body.contains("messages")) {
            for (const auto& msg : body["messages"]) {
                inferdeck::core::MessageRole role = inferdeck::core::MessageRole::User;
                if (msg.contains("role")) {
                    auto r = msg["role"].get<std::string>();
                    if (r == "system") role = inferdeck::core::MessageRole::System;
                    else if (r == "assistant") role = inferdeck::core::MessageRole::Assistant;
                    else if (r == "tool") role = inferdeck::core::MessageRole::Tool;
                }
                std::string content;
                auto& c = msg["content"];
                if (c.is_string()) {
                    content = c.get<std::string>();
                } else if (c.is_array()) {
                    for (const auto& part : c) {
                        if (part.contains("type") && part["type"] == "text" && part.contains("text")) {
                            content += part["text"].get<std::string>();
                        } else if (part.is_string()) {
                            content += part.get<std::string>();
                        }
                    }
                }
                std::string tool_call_id = msg.value("tool_call_id", "");
                std::string name = msg.value("name", "");
                std::string tool_calls_json;
                if (role == inferdeck::core::MessageRole::Assistant && msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                    tool_calls_json = msg["tool_calls"].dump();
                }
                messages.push_back({role, content, tool_call_id, name, tool_calls_json});
            }
        }

        inferdeck::core::InferenceParams params;
        params.max_tokens = body.value("max_tokens", -1);
        if (body.contains("temperature")) params.temperature = body["temperature"].get<float>();
        if (body.contains("top_p")) params.top_p = body["top_p"].get<float>();
        if (body.contains("stop")) {
            if (body["stop"].is_string()) params.stop = body["stop"].get<std::string>();
        }
        const std::string accepted_client = SafeLogText(DetectChatClientName(req), 80);
        const std::string request_protocol = SafeLogText(req.get_header_value("X-InferDeck-Protocol").empty()
            ? "/v1/chat/completions"
            : req.get_header_value("X-InferDeck-Protocol"), 80);
        const bool request_has_tools = ShouldForceNonStreamingBackend(body);
        bool force_non_streaming_backend = request_has_tools;
        if (body.contains("tools") && body["tools"].is_array()) {
            params.tools_json = body["tools"].dump();
        }
        stream = ShouldUseStreamingBackendForClient(body, accepted_client);

        auto& activity = inferdeck::gateway::RuntimeActivity::Get();
        const std::string accepted_model = body.value("model", "local-model");
        std::string job_id = activity.StartJob(
            request_has_tools ? "tool_chat.completion" : "chat.completion",
            accepted_client,
            accepted_model,
            BuildJobPayloadPreview(body, client_requested_stream, force_non_streaming_backend, request_protocol),
            request_has_tools ? 80 : 50
        );
        resp.set_header("X-InferDeck-Job-Id", job_id);
        const std::string user_agent = SafeLogText(req.get_header_value("User-Agent"));
        const std::string remote_addr = SafeLogText(req.remote_addr, 64);
        inferdeck::core::Logger::Get().Info(
            "Accepted /v1/chat/completions request id=" + job_id +
            " client=" + accepted_client +
            " protocol=" + request_protocol +
            " remote=" + remote_addr +
            " userAgent=" + user_agent +
            " model=" + accepted_model +
            " stream=" + std::string(client_requested_stream ? "true" : "false") +
            " tools=" + std::to_string(body.contains("tools") && body["tools"].is_array() ? body["tools"].size() : 0) +
            " messages=" + std::to_string(body.contains("messages") && body["messages"].is_array() ? body["messages"].size() : 0)
        );
        inferdeck::core::Logger::Get().Info("Request " + job_id + " entering model selection");

        std::unique_lock<std::mutex> model_lock(g_switch_mutex);
        std::string current_clean_id = GetCurrentCleanModelId();
        std::string response_model_id = current_clean_id;

        std::string requested_model = body.value("model", "");
        if (!requested_model.empty()) {
            current_clean_id = GetCurrentCleanModelId();
            response_model_id = current_clean_id;

            std::string normalized_requested = NormalizeId(requested_model);
            std::string normalized_current = NormalizeId(current_clean_id);
            inferdeck::core::Logger::Get().Info("Model request normalized: requested='" + normalized_requested + "' current='" + normalized_current + "'");

            bool needs_switch = (normalized_requested != normalized_current);

            if (needs_switch) {
                std::string model_path = FindModelPath(requested_model);
                if (model_path.empty() && normalized_requested == "gpt-oss-20b") {
                    std::filesystem::path gpt_path = "C:/Users/david/Documents/00_Models/openai_gpt-oss-20b-GGUF/openai_gpt-oss-20b-MXFP4.gguf";
                    if (std::filesystem::exists(gpt_path)) model_path = gpt_path.string();
                }
                if (!model_path.empty()) {
                    inferdeck::core::Logger::Get().Info("Switching model for request '" + requested_model + "' -> " + model_path);
                    if (engine.SwitchModel(model_path)) {
                        response_model_id = MakeCleanModelId(model_path);
                    } else {
                        inferdeck::core::Logger::Get().Error("Failed to switch model for request '" + requested_model + "'");
                    }
                } else {
                    inferdeck::core::Logger::Get().Warn("Requested model not found in model directory: " + requested_model);
                    response_model_id = requested_model;
                }
            }
        }

        if (stream) {
            inferdeck::core::Logger::Get().Info("Request " + job_id + " using backend streaming SSE");
            std::string id = MakeId();
            auto started = std::make_shared<std::atomic<bool>>(false);
            auto lock_holder = std::make_shared<std::unique_lock<std::mutex>>(std::move(model_lock));

            resp.set_chunked_content_provider(
                "text/event-stream",
                [&engine, messages, params, id, response_model_id, job_id, started, lock_holder, request_protocol](size_t, httplib::DataSink& sink) -> bool {
                    bool expected = false;
                    if (!started->compare_exchange_strong(expected, true)) {
                        sink.done();
                        return false;
                    }
                    std::uint64_t sse_chunks_sent = 0;
                    std::uint64_t sse_bytes_sent = 0;
                    std::uint64_t heartbeat_chunks_sent = 0;
                    std::atomic<bool> client_disconnected{false};
                    auto write_chunk = [&](const std::string& chunk) {
                        if (client_disconnected.load()) return false;
                        sse_chunks_sent += CountSseDataChunks(chunk);
                        sse_bytes_sent += chunk.size();
                        const bool ok = sink.write(chunk.data(), chunk.size());
                        if (!ok) client_disconnected.store(true);
                        return ok;
                    };
                    auto write_heartbeat = [&]() {
                        const std::string heartbeat = ": inferdeck backend still running\n\n";
                        if (!write_chunk(heartbeat)) {
                            inferdeck::gateway::RuntimeActivity::Get().FailJob(job_id, "stream client disconnected", 499);
                            inferdeck::core::LlamaEngine::Get().AbortActiveRequest("stream client disconnected");
                            return;
                        }
                        ++heartbeat_chunks_sent;
                    };
                    try {
                        inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, json({
                            {"requestProtocol", request_protocol},
                            {"responseProtocol", "openai.sse"},
                            {"responseMode", "backend-stream"},
                            {"waitingOnBackendOrToolFormatting", true}
                        }), "OpenAI SSE backend stream started");
                        auto role_chunk = SseDeltaChunk(id, response_model_id, json({{"role", "assistant"}}));
                        if (!write_chunk(role_chunk)) {
                            inferdeck::gateway::RuntimeActivity::Get().FailJob(job_id, "stream client disconnected", 499);
                            inferdeck::core::LlamaEngine::Get().AbortActiveRequest("stream client disconnected");
                            sink.done();
                            return false;
                        }
                        std::string streamed_content;
                        std::size_t streamed_content_sent = 0;
                        auto on_token = [&](const std::string& token, inferdeck::core::TokenType type, int) {
                            std::string chunk;
                            if (type == inferdeck::core::TokenType::Content) {
                                streamed_content += token;
                                const auto clean_content = SanitizeAssistantContent(streamed_content);
                                if (clean_content.size() < streamed_content_sent) {
                                    streamed_content_sent = clean_content.size();
                                }
                                if (clean_content.size() > streamed_content_sent) {
                                    chunk = SseChunk(id, response_model_id, clean_content.substr(streamed_content_sent), "", false);
                                    streamed_content_sent = clean_content.size();
                                }
                            } else if (type == inferdeck::core::TokenType::Reasoning) {
                                chunk = SseChunk(id, response_model_id, "", token, false);
                            }
                            if (!chunk.empty() && !write_chunk(chunk)) {
                                inferdeck::gateway::RuntimeActivity::Get().FailJob(job_id, "stream client disconnected", 499);
                                inferdeck::core::LlamaEngine::Get().AbortActiveRequest("stream client disconnected");
                            }
                        };
                        auto result = engine.PredictStream(messages, params, on_token, write_heartbeat);
                        if (lock_holder->owns_lock()) lock_holder->unlock();
                        inferdeck::gateway::RuntimeActivity::Get().CompleteJob(job_id, result, json({
                            {"requestProtocol", request_protocol},
                            {"responseProtocol", "openai.sse"},
                            {"responseMode", "backend-stream"},
                            {"streamed", true},
                            {"sseChunks", sse_chunks_sent},
                            {"heartbeatChunks", heartbeat_chunks_sent},
                            {"responseBytes", sse_bytes_sent},
                            {"finishReason", result.HasError() ? "error" : "stop"},
                            {"toolCallCount", result.tool_calls.size()},
                            {"waitingOnBackendOrToolFormatting", false},
                            {"contentPreview", result.text.substr(0, 240)},
                            {"reasoningPreview", result.reasoning_text.substr(0, 240)}
                        }));
                        if (result.HasError()) write_chunk(SseErrorChunk(id, response_model_id, result.error_message));
                        write_chunk(SseChunk(id, response_model_id, "", "", true));
                        write_chunk("data: [DONE]\n\n");
                    } catch (const std::exception& e) {
                        if (lock_holder->owns_lock()) lock_holder->unlock();
                        inferdeck::gateway::RuntimeActivity::Get().FailJob(job_id, e.what());
                        write_chunk(SseErrorChunk(id, response_model_id, e.what()));
                        write_chunk(SseChunk(id, response_model_id, "", "", true));
                        write_chunk("data: [DONE]\n\n");
                    }
                    if (lock_holder->owns_lock()) lock_holder->unlock();
                    sink.done();
                    return false;
                }
            );
            resp.set_header("Cache-Control", "no-cache");
            resp.set_header("Connection", "keep-alive");
            return;
        }

        if (client_requested_stream && force_non_streaming_backend) {
            const std::string id = MakeId();
            auto started = std::make_shared<std::atomic<bool>>(false);
            const std::string log_label = request_has_tools ? "tool request" : "plain request";
            if (model_lock.owns_lock()) model_lock.unlock();

            resp.set_chunked_content_provider(
                "text/event-stream",
                [&engine, messages, params, id, response_model_id, job_id, started, log_label, request_protocol](size_t, httplib::DataSink& sink) -> bool {
                    bool expected = false;
                    if (!started->compare_exchange_strong(expected, true)) {
                        sink.done();
                        return false;
                    }
                    inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, json({
                        {"requestProtocol", request_protocol},
                        {"responseProtocol", "openai.sse"},
                        {"responseMode", "guarded-synthetic-sse"},
                        {"waitingOnBackendOrToolFormatting", true}
                    }), "Guarded synthetic SSE waiting on backend/tool formatting");
                    return RunGuardedSyntheticSse(engine, messages, params, id, response_model_id, job_id, log_label, request_protocol, sink);
                }
            );
            resp.set_header("Cache-Control", "no-cache");
            resp.set_header("Connection", "keep-alive");
            return;
        }

        auto result = engine.Predict(messages, params);
        inferdeck::core::Logger::Get().Info("Request " + job_id + " backend non-streaming completed");
        model_lock.unlock();
        if (result.HasError()) {
            activity.CompleteJob(job_id, result, json({{"error", result.error_message}}));
            SetInferenceError(resp, result);
            return;
        }

        json response = BuildChatCompletionResponse(MakeId(), response_model_id, result);
        activity.CompleteJob(job_id, result, json({
            {"requestProtocol", request_protocol},
            {"responseProtocol", client_requested_stream ? "openai.sse" : "openai.json"},
            {"responseMode", "json"},
            {"sseChunks", 0},
            {"responseBytes", response.dump().size()},
            {"finishReason", ResponseFinishReason(response)},
            {"toolCallCount", ResponseToolCallCount(response)},
            {"waitingOnBackendOrToolFormatting", false},
            {"contentPreview", result.text.substr(0, 500)},
            {"reasoningPreview", result.reasoning_text.substr(0, 500)},
            {"toolCalls", response["choices"][0]["message"].contains("tool_calls") ? response["choices"][0]["message"]["tool_calls"] : json::array()},
            {"usage", response["usage"]}
        }));
        if (client_requested_stream) {
            resp.set_header("Cache-Control", "no-cache");
            resp.set_header("Connection", "keep-alive");
            resp.set_content(BuildSyntheticChatCompletionStream(response), "text/event-stream");
            return;
        }

        resp.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        resp.status = 400;
        resp.set_content(json({{"error", {{"message", e.what()}, {"type", "invalid_request_error"}, {"code", 400}}}}).dump(), "application/json");
    }
}

void HandleChatCompletionsStream(const httplib::Request& req, httplib::Response& resp) {
    HandleChatCompletions(req, resp);
}

} // namespace
