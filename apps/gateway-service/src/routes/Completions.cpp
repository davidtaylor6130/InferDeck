/// @file Completions.cpp
/// @brief /v1/completions route handler implementation.

#include "routes/Completions.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "core/Logger.hpp"
#include "core/Metrics.hpp"

#include <nlohmann/json.hpp>

namespace inferdeck::gateway::routes {

void HandleCompletions(const httplib::Request& req, httplib::Response& resp) {
    std::string body = req.body;
    if (body.empty()) {
        nlohmann::json error;
        error["error"]["message"] = "Request body is required";
        error["error"]["type"] = "invalid_request_error";
        resp.set_content(error.dump(2), "application/json");
        resp.status = 400;
        return;
    }

    nlohmann::json j = nlohmann::json::parse(body);

    int max_tokens = j.value("max_tokens", 256);
    float temperature = j.value("temperature", 0.7f);
    std::string prompt = j.value("prompt", "");

    if (prompt.empty()) {
        nlohmann::json error;
        error["error"]["message"] = "Missing 'prompt' field";
        error["error"]["type"] = "invalid_request_error";
        resp.set_content(error.dump(2), "application/json");
        resp.status = 400;
        return;
    }

    auto& engine = inferdeck::core::LlamaEngine::Get();
    if (!engine.IsInitialized()) {
        nlohmann::json error;
        error["error"]["message"] = "Model not loaded";
        error["error"]["type"] = "service_unavailable";
        resp.set_content(error.dump(2), "application/json");
        resp.status = 503;
        return;
    }

    // Convert to chat format internally
    std::vector<inferdeck::core::ChatMessage> messages;
    messages.push_back({inferdeck::core::MessageRole::User, prompt});

    inferdeck::core::InferenceParams params;
    params.max_tokens = max_tokens;
    params.temperature = temperature;

    auto result = engine.Predict(messages, params);

    nlohmann::json response;
    response["id"] = "cmpl-" + std::to_string(std::time(nullptr));
    response["object"] = "text_completion";
    response["created"] = static_cast<int>(std::time(nullptr));
    response["model"] = j.value("model", "default");

    nlohmann::json choice;
    choice["text"] = result.text;
    choice["index"] = 0;
    choice["finish_reason"] = "stop";
    response["choices"].push_back(choice);

    nlohmann::json usage;
    usage["prompt_tokens"] = result.prompt_tokens;
    usage["completion_tokens"] = result.completion_tokens;
    usage["total_tokens"] = result.total_tokens;
    response["usage"] = usage;

    resp.set_content(response.dump(2), "application/json");
    resp.status = 200;

    MetricsStore::Get().IncrementCounter("inferdeck.requests.total", 1);
    MetricsStore::Get().IncrementCounter("inferdeck.requests.success", 1);
}

void HandleCompletionsStream(const httplib::Request& req, httplib::Response& resp) {
    // Similar to HandleCompletions but with SSE output
    // For V1, redirect to non-streaming handler
    HandleCompletions(req, resp);
}

} // namespace inferdeck::gateway::routes
