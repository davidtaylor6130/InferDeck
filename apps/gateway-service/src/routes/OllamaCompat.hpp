#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace inferdeck::gateway::routes {
void HandleOllamaVersion(const httplib::Request& req, httplib::Response& resp);
void HandleOllamaTags(const httplib::Request& req, httplib::Response& resp);
void HandleOllamaChat(const httplib::Request& req, httplib::Response& resp);
nlohmann::json BuildOpenAiChatBodyFromOllama(const nlohmann::json& ollama);
nlohmann::json BuildOllamaChatResponseFromOpenAi(const nlohmann::json& openai, const std::string& model);
std::string BuildOllamaChatStreamFromOpenAiResponse(const nlohmann::json& openai, const std::string& model);
std::string BuildOllamaChatStreamFromOpenAiSse(const std::string& openai_sse, const std::string& model);
}
