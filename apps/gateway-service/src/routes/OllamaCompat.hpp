#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace inferdeck::gateway::routes {
void HandleOllamaVersion(const httplib::Request& req, httplib::Response& resp);
void HandleOllamaTags(const httplib::Request& req, httplib::Response& resp);
void HandleOllamaChat(const httplib::Request& req, httplib::Response& resp);
nlohmann::json BuildOpenAiChatBodyFromOllama(const nlohmann::json& ollama);
std::string BuildOllamaChatStreamFromOpenAiSse(const std::string& openai_sse, const std::string& model);
}
