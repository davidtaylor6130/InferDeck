/// @file ChatCompletions.cpp
/// @brief /v1/chat/completions route handler implementation.

#include "routes/ChatCompletions.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "core/Logger.hpp"
#include "core/Metrics.hpp"

#include <nlohmann/json.hpp>
#include <chrono>

namespace inferdeck::gateway::routes {

std::string ValidateChatRequest(const std::string& body) {
    // For V1, basic validation
    // TODO: Add full OpenAI schema validation
    if (body.empty()) {
        return "Request body is required";
    }

    try {
        nlohmann::json j = nlohmann::json::parse(body);
        if (!j.contains("messages") || !j["messages"].is_array()) {
            return "Missing or invalid 'messages' field";
        }
        if (j["messages"].empty()) {
            return "'messages' array must not be empty";
        }
        // Validate each message has role and content
        for (const auto& msg : j["messages"]) {
            if (!msg.contains("role") || !msg.contains("content")) {
                return "Each message must have 'role' and 'content'";
            }
            std::string role = msg["role"];
            if (role != "system" && role != "user" && role != "assistant" && role != "tool") {
                return "Invalid role: " + role;
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        return "Invalid JSON: " + std::string(e.what());
    }

    return "";
}

void HandleChatCompletions(const httplib::Request& req, httplib::Response& resp) {
    // Parse request
    std::string body = req.body;
    std::string validation_error = ValidateChatRequest(body);
    if (!validation_error.empty()) {
        nlohmann::json error;
        error["error"]["message"] = validation_error;
        error["error"]["type"] = "invalid_request_error";
        resp.set_content(error.dump(2), "application/json");
        resp.status = 400;
        return;
    }

    nlohmann::json j = nlohmann::json::parse(body);

    // Extract parameters
    int max_tokens = j.value("max_tokens", 256);
    float temperature = j.value("temperature", 0.7f);
    float top_p = j.value("top_p", 0.9f);
    bool stream = j.value("stream", false);

    // Handle streaming case
    if (stream) {
        // Redirect to streaming handler
        HandleChatCompletionsStream(req, resp);
        return;
    }

    // Convert messages to internal format
    std::vector<inferdeck::core::ChatMessage> messages;
    for (const auto& msg : j["messages"]) {
        std::string role = msg["role"];
        std::string content = msg.value("content", "");
        inferdeck::core::MessageRole role_enum;
        if (role == "system") role_enum = inferdeck::core::MessageRole::System;
        else if (role == "user") role_enum = inferdeck::core::MessageRole::User;
        else if (role == "assistant") role_enum = inferdeck::core::MessageRole::Assistant;
        else role_enum = inferdeck::core::MessageRole::Tool;

        messages.push_back({role_enum, content});
    }

    // Run inference
    auto& engine = inferdeck::core::LlamaEngine::Get();
    if (!engine.IsInitialized()) {
        nlohmann::json error;
        error["error"]["message"] = "Model not loaded";
        error["error"]["type"] = "service_unavailable";
        resp.set_content(error.dump(2), "application/json");
        resp.status = 503;
        return;
    }

    inferdeck::core::InferenceParams params;
    params.max_tokens = max_tokens;
    params.temperature = temperature;
    params.top_p = top_p;

    auto result = engine.Predict(messages, params);

    // Build response
    nlohmann::json response;
    response["id"] = "chatcmpl-" + std::to_string(std::time(nullptr));
    response["object"] = "chat.completion";
    response["created"] = static_cast<int>(std::time(nullptr));
    response["model"] = j.value("model", "default");

    nlohmann::json choice;
    choice["index"] = 0;
    choice["message"]["role"] = "assistant";
    choice["message"]["content"] = result.text;
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

void HandleChatCompletionsStream(const httplib::Request& req, httplib::Response& resp) {
    // Parse request
    std::string body = req.body;
    std::string validation_error = ValidateChatRequest(body);
    if (!validation_error.empty()) {
        resp.status = 400;
        return;
    }

    nlohmann::json j = nlohmann::json::parse(body);

    int max_tokens = j.value("max_tokens", 256);
    float temperature = j.value("temperature", 0.7f);
    float top_p = j.value("top_p", 0.9f);

    // Convert messages
    std::vector<inferdeck::core::ChatMessage> messages;
    for (const auto& msg : j["messages"]) {
        std::string role = msg["role"];
        std::string content = msg.value("content", "");
        inferdeck::core::MessageRole role_enum;
        if (role == "system") role_enum = inferdeck::core::MessageRole::System;
        else if (role == "user") role_enum = inferdeck::core::MessageRole::User;
        else if (role == "assistant") role_enum = inferdeck::core::MessageRole::Assistant;
        else role_enum = inferdeck::core::MessageRole::Tool;
        messages.push_back({role_enum, content});
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

    // Set SSE headers
    resp.set_header("Content-Type", "text/event-stream");
    resp.set_header("Cache-Control", "no-cache");
    resp.set_header("Connection", "keep-alive");
    resp.status = 200;

    // Streaming callback
    std::string chat_id = "chatcmpl-" + std::to_string(std::time(nullptr));
    int token_count = 0;

    auto on_token = [&](const std::string& token, int cumulative) {
        nlohmann::json chunk;
        chunk["id"] = chat_id;
        chunk["object"] = "chat.completion.chunk";
        chunk["created"] = static_cast<int>(std::time(nullptr));
        chunk["model"] = j.value("model", "default");

        nlohmann::json delta;
        if (cumulative == 0) {
            delta["role"] = "assistant";
        }
        delta["content"] = token;

        nlohmann::json choice;
        choice["index"] = 0;
        choice["delta"] = delta;
        choice["finish_reason"] = nlohmann::json::value_t::null;
        chunk["choices"].push_back(choice);

        resp.stream_chunk("\n" + chunk.dump(2));

        token_count++;
        if (token_count >= max_tokens) {
            // Send [DONE]
            resp.stream_chunk("\ndata: [DONE]\n\n");
            resp.flush();
        }
    };

    inferdeck::core::InferenceParams params;
    params.max_tokens = max_tokens;
    params.temperature = temperature;
    params.top_p = top_p;
    params.stream = true;

    engine.PredictStream(messages, params, on_token);

    // Send final [DONE] if not already sent
    if (token_count < max_tokens) {
        resp.stream_chunk("\ndata: [DONE]\n\n");
    }
    resp.flush();
}

} // namespace inferdeck::gateway::routes
