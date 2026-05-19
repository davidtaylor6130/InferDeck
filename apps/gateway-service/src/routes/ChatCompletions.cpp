#include "routes/ChatCompletions.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <chrono>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <filesystem>
#include <vector>
using json = nlohmann::json;

namespace {

static std::string MakeModelId(const std::string& full_path) {
    if (full_path.empty()) return "local-model";
    std::string name = full_path;
    std::vector<std::string> extensions = {".gguf", ".bin", ".ggml", ".pt", ".pth"};
    for (const auto& ext : extensions) {
        if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            name = name.substr(0, name.size() - ext.size());
            break;
        }
    }
    for (auto& c : name) {
        if (c == ' ' || c == '_' || c == '-') c = '-';
        else c = std::tolower(static_cast<unsigned char>(c));
    }
    return name;
}

}

namespace inferdeck::gateway::routes {

static std::string MakeId() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return "chatcmpl-" + std::to_string(ms);
}

static std::string SseChunk(const std::string& id, const std::string& model, const std::string& content, bool done) {
    json chunk;
    chunk["id"] = id;
    chunk["object"] = "chat.completion.chunk";
    chunk["model"] = model;
    chunk["created"] = std::time(nullptr);
    if (done) {
        chunk["choices"] = json::array({{{"index", 0}, {"delta", json::object()}, {"finish_reason", "stop"}}});
    } else {
        chunk["choices"] = json::array({{{"index", 0}, {"delta", {{"role", "assistant"}, {"content", content}}}, {"finish_reason", nullptr}}});
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
                messages.push_back({role, msg.value("content", "")});
            }
        }

        inferdeck::core::InferenceParams params;
        if (body.contains("max_tokens")) params.max_tokens = body["max_tokens"].get<int>();
        if (body.contains("temperature")) params.temperature = body["temperature"].get<float>();
        if (body.contains("top_p")) params.top_p = body["top_p"].get<float>();
        if (body.contains("stop")) {
            if (body["stop"].is_string()) params.stop = body["stop"].get<std::string>();
        }

        std::string model_id = MakeModelId(engine.GetModelName());

        if (stream) {
            std::string id = MakeId();
            auto queue = std::make_shared<StreamQueue>();

            std::thread predict_thread([&engine, messages, params, queue, model_id, id]() {
                auto on_token = [queue, id, model_id](const std::string& token, int) {
                    queue->push(SseChunk(id, model_id, token, false));
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
        response["choices"] = json::array({{{"index", 0}, {"message", {{"role", "assistant"}, {"content", result.text}}}, {"finish_reason", "stop"}}});
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
