#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>
namespace inferdeck::gateway::routes {
void HandleChatCompletions(const httplib::Request& req, httplib::Response& resp);
void HandleChatCompletionsStream(const httplib::Request& req, httplib::Response& resp);

std::string ValidateChatRequest(const std::string& body);
std::string BuildSyntheticChatCompletionStream(const nlohmann::json& response);
std::string BuildSyntheticChatCompletionStream(const nlohmann::json& response, const nlohmann::json& request_tools);
nlohmann::json BuildChatCompletionResponseForTest(const std::string& id,
                                                  const std::string& model,
                                                  const std::string& content,
                                                  const nlohmann::json& request_tools);
nlohmann::json BuildChatCompletionResponseForTest(const std::string& id,
                                                  const std::string& model,
                                                  const std::string& content,
                                                  const std::string& reasoning,
                                                  const nlohmann::json& request_tools);
nlohmann::json NormalizeOpenAiToolCalls(const nlohmann::json& tool_calls);
std::string CompactToolResultContentForTest(const std::string& content);
bool ShouldForceNonStreamingBackend(const nlohmann::json& request);
bool ShouldUseStreamingBackend(const nlohmann::json& request);
bool ShouldUseStreamingBackendForClient(const nlohmann::json& request, const std::string& client);
std::string DetectChatClientName(const httplib::Request& req);
std::string SanitizeAssistantContent(const std::string& content);
std::string ExtractAssistantReasoningContent(const std::string& content);
}
