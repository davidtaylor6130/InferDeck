/// @file ChatCompletions.hpp
/// @brief /v1/chat/completions route handler.

#pragma once

#include <httplib.h>

namespace inferdeck::gateway::routes {

/// Handle /v1/chat/completions POST request.
void HandleChatCompletions(const httplib::Request& req, httplib::Response& resp);

/// Handle streaming /v1/chat/completions request.
void HandleChatCompletionsStream(const httplib::Request& req, httplib::Response& resp);

/// Validate the chat completions request body.
std::string ValidateChatRequest(const std::string& body);

} // namespace inferdeck::gateway::routes
