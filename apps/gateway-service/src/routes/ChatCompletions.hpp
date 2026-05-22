#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>
namespace inferdeck::gateway::routes {
void HandleChatCompletions(const httplib::Request& req, httplib::Response& resp);
void HandleChatCompletionsStream(const httplib::Request& req, httplib::Response& resp);

std::string ValidateChatRequest(const std::string& body);
std::string BuildSyntheticChatCompletionStream(const nlohmann::json& response);
bool ShouldForceNonStreamingBackend(const nlohmann::json& request);
std::string SanitizeAssistantContent(const std::string& content);
std::string ExtractAssistantReasoningContent(const std::string& content);
}
