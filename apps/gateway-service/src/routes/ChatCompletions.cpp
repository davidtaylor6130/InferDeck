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

static std::unordered_map<std::string, std::string> g_model_cache;
static std::mutex g_model_cache_mutex;

static std::string FindModelPath(const std::string& model_id) {
    std::lock_guard<std::mutex> lock(g_model_cache_mutex);
    if (!g_model_cache.empty()) {
        auto it = g_model_cache.find(model_id);
        if (it != g_model_cache.end()) return it->second;
    }

    std::string model_dir;
    try {
        auto full = inferdeck::core::Config::Load(inferdeck::gateway::GetDefaultConfigPath());
        model_dir = full.model.directory;
    } catch (...) { return ""; }

    if (model_dir.empty() || !std::filesystem::exists(model_dir)) return "";

    std::vector<std::string> extensions = {".gguf", ".bin", ".ggml"};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(model_dir)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            std::string filename = entry.path().filename().string();
            if (filename.find("mmproj") == 0) continue;
            for (const auto& e : extensions) {
                if (ext == e) {
                    std::string id = MakeModelId(entry.path().string());
                    g_model_cache[id] = entry.path().string();
                    if (id == model_id) return entry.path().string();
                }
            }
        }
    }
    return "";
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
                messages.push_back({role, content});
            }
        }

        inferdeck::core::InferenceParams params;
        if (body.contains("max_tokens")) params.max_tokens = body["max_tokens"].get<int>();
        if (body.contains("temperature")) params.temperature = body["temperature"].get<float>();
        if (body.contains("top_p")) params.top_p = body["top_p"].get<float>();
        if (body.contains("stop")) {
            if (body["stop"].is_string()) params.stop = body["stop"].get<std::string>();
        }

        std::string requested_model = body.value("model", "");
        std::string model_id = MakeModelId(engine.GetModelName());

        if (!requested_model.empty() && requested_model != model_id) {
            std::string model_path = FindModelPath(requested_model);
            if (!model_path.empty()) {
                engine.SwitchModel(model_path);
                model_id = requested_model;
            }
        }

        if (stream) {
            std::string id = MakeId();
            auto queue = std::make_shared<StreamQueue>();

            std::thread predict_thread([&engine, messages, params, queue, model_id, id]() {
                auto on_token = [queue, id, model_id](const std::string& token, inferdeck::core::TokenType type, int) {
                    if (type == inferdeck::core::TokenType::Content) {
                        queue->push(SseChunk(id, model_id, token, "", false));
                    } else if (type == inferdeck::core::TokenType::Reasoning) {
                        queue->push(SseChunk(id, model_id, "", token, false));
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
        response["model"] = model_id;
        json message = {{"role", "assistant"}, {"content", result.text}};
        if (!result.reasoning_text.empty()) {
            message["reasoning_content"] = result.reasoning_text;
        }
        response["choices"] = json::array({{{"index", 0}, {"message", message}, {"finish_reason", "stop"}}});
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
