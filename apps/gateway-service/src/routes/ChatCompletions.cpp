#include "routes/ChatCompletions.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "config/ConfigLoader.hpp"
#include "core/Config.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <chrono>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <filesystem>
#include <vector>
#include <unordered_map>
using json = nlohmann::json;

namespace {

static std::string MakeModelId(const std::string& full_path) {
    if (full_path.empty()) return "local-model";
    std::filesystem::path p(full_path);
    std::string name = p.stem().string();
    for (auto& c : name) {
        if (c == ' ' || c == '_' || c == '-') c = '-';
        else c = std::tolower(static_cast<unsigned char>(c));
    }
    return name;
}

static std::string MakeCleanModelId(const std::string& full_path) {
    if (full_path.empty()) return "local-model";
    std::filesystem::path p(full_path);
    std::string name = p.stem().string();

    static const std::vector<std::string> quant_suffixes = {
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

    std::string lower = name;
    for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));

    std::string clean = name;
    for (const auto& suffix : quant_suffixes) {
        size_t pos = lower.rfind(suffix);
        if (pos != std::string::npos && pos + suffix.size() == lower.size()) {
            clean = name.substr(0, pos);
            break;
        }
    }

    for (auto& c : clean) {
        if (c == ' ' || c == '_' || c == '-') c = '-';
        else c = std::tolower(static_cast<unsigned char>(c));
    }

    return clean;
}

static std::string NormalizeId(const std::string& id) {
    std::string result = id;
    for (auto& c : result) {
        if (c == ' ' || c == '_') c = '-';
        else c = std::tolower(static_cast<unsigned char>(c));
    }
    if (result.size() > 8 && result.compare(result.size() - 8, 8, ":latest") == 0) {
        result = result.substr(0, result.size() - 8);
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

    return "";
}

static std::string GetCurrentCleanModelId() {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    std::string model_name = engine.GetModelName();
    if (model_name.empty()) return "";
    return MakeCleanModelId(model_name);
}

}

namespace inferdeck::gateway::routes {

static std::string MakeId() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return "chatcmpl-" + std::to_string(ms);
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

    void finish() {
        done.store(true);
        cv.notify_all();
    }
};

void HandleChatCompletions(const httplib::Request& req, httplib::Response& resp) {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    if (!engine.IsInitialized()) {
        resp.status = 503;
        resp.set_content(R"({"error":{"message":"Engine not initialized","type":"service_error","code":503}})", "application/json");
        return;
    }
    try {
        auto body = json::parse(req.body);
        bool stream = body.value("stream", false);

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
        if (body.contains("tools") && body["tools"].is_array()) {
            params.tools_json = body["tools"].dump();
        }

        std::string requested_model = body.value("model", "");
        std::string current_clean_id = GetCurrentCleanModelId();
        std::string response_model_id = current_clean_id;

        if (!requested_model.empty()) {
            std::string normalized_requested = NormalizeId(requested_model);
            std::string normalized_current = NormalizeId(current_clean_id);

            bool needs_switch = (normalized_requested != normalized_current);

            if (needs_switch) {
                std::string model_path = FindModelPath(requested_model);
                if (!model_path.empty()) {
                    engine.SwitchModel(model_path);
                    response_model_id = MakeCleanModelId(model_path);
                } else {
                    response_model_id = requested_model;
                }
            } else {
                response_model_id = current_clean_id;
            }
        }

        if (stream) {
            std::string id = MakeId();
            auto queue = std::make_shared<StreamQueue>();

            std::thread predict_thread([&engine, messages, params, queue, response_model_id, id]() {
                auto on_token = [queue, id, response_model_id](const std::string& token, inferdeck::core::TokenType type, int) {
                    if (type == inferdeck::core::TokenType::Content) {
                        queue->push(SseChunk(id, response_model_id, token, "", false));
                    } else if (type == inferdeck::core::TokenType::Reasoning) {
                        queue->push(SseChunk(id, response_model_id, "", token, false));
                    }
                };
                engine.PredictStream(messages, params, on_token);
                queue->push("data: [DONE]\n\n");
                queue->finish();
            });

            resp.set_content_provider(
                "text/event-stream",
                [queue](size_t, httplib::DataSink& sink) -> bool {
                    std::string chunk;
                    if (queue->pop(chunk)) {
                        sink.write(chunk.data(), chunk.size());
                        return true;
                    }
                    sink.done();
                    return false;
                }
            );

            predict_thread.detach();
            return;
        }

        auto result = engine.Predict(messages, params);
        json response;
        response["id"] = MakeId();
        response["object"] = "chat.completion";
        response["created"] = std::time(nullptr);
        response["model"] = response_model_id;
        json message = {{"role", "assistant"}};
        if (!result.text.empty()) {
            message["content"] = result.text;
        } else if (result.tool_calls.empty()) {
            message["content"] = "";
        }
        if (!result.reasoning_text.empty()) {
            message["reasoning_content"] = result.reasoning_text;
        }
        if (!result.tool_calls.empty()) {
            json tool_calls = json::array();
            for (const auto& tc : result.tool_calls) {
                json tc_json;
                tc_json["id"] = tc.id;
                tc_json["type"] = tc.type;
                tc_json["function"] = {{"name", tc.function_name}, {"arguments", tc.function_arguments}};
                tool_calls.push_back(tc_json);
            }
            message["tool_calls"] = tool_calls;
        }
        std::string finish_reason = "stop";
        if (!result.tool_calls.empty()) finish_reason = "tool_calls";
        response["choices"] = json::array({{{"index", 0}, {"message", message}, {"finish_reason", finish_reason}}});
        response["usage"] = {{"prompt_tokens", result.prompt_tokens}, {"completion_tokens", result.completion_tokens}, {"total_tokens", result.total_tokens}};
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
