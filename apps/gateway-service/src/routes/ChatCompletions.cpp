#include "routes/ChatCompletions.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "core/Logger.hpp"
#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace inferdeck::gateway::routes {

void HandleChatCompletions(const httplib::Request& req, httplib::Response& resp) {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    if (!engine.IsInitialized()) {
        resp.status = 503;
        resp.set_content(R"({"error":"Engine not initialized"})", "application/json");
        return;
    }
    try {
        auto body = json::parse(req.body);
        std::vector<inferdeck::core::ChatMessage> messages;
        if (body.contains("messages")) {
            for (const auto& msg : body["messages"]) {
                inferdeck::core::MessageRole role = inferdeck::core::MessageRole::User;
                if (msg.contains("role")) {
                    auto r = msg["role"].get<std::string>();
                    if (r == "system") role = inferdeck::core::MessageRole::System;
                    else if (r == "assistant") role = inferdeck::core::MessageRole::Assistant;
                }
                messages.push_back({role, msg.value("content", "")});
            }
        }
        inferdeck::core::InferenceParams params;
        if (body.contains("max_tokens")) params.max_tokens = body["max_tokens"].get<int>();
        if (body.contains("temperature")) params.temperature = body["temperature"].get<float>();
        auto result = engine.Predict(messages, params);
        json response;
        response["id"] = "chatcmpl-1";
        response["object"] = "chat.completion";
        response["created"] = std::time(nullptr);
        response["model"] = engine.GetModelName();
        response["choices"] = json::array({{{"index", 0}, {"message", {{"role", "assistant"}, {"content", result.text}}}, {"finish_reason", "stop"}}});
        response["usage"] = {{"prompt_tokens", result.prompt_tokens}, {"completion_tokens", result.completion_tokens}, {"total_tokens", result.total_tokens}};
        resp.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        resp.status = 400;
        resp.set_content(json({{"error", e.what()}}).dump(), "application/json");
    }
}

void HandleChatCompletionsStream(const httplib::Request& req, httplib::Response& resp) {
    resp.status = 501;
    resp.set_content(R"({"error":"Streaming not yet implemented"})", "application/json");
}

} // namespace
