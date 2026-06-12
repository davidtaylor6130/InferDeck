#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gateway/routes.hpp"

#include <string>

namespace inferdeck::gateway {

// Resolve an Anthropic-style model name (e.g. "claude-sonnet-4-6") to a
// registry model: explicit alias > exact registry match > default model.
std::string resolve_anthropic_model(const GatewayDeps& deps, const std::string& requested);

// Translate an Anthropic Messages API request body into an OpenAI
// chat-completions body (the format the inference pipeline consumes).
// Throws nlohmann::json exceptions on malformed input.
nlohmann::json anthropic_to_openai(const nlohmann::json& body, const std::string& resolved_model);

// Map an internal finish_reason to an Anthropic stop_reason.
std::string anthropic_stop_reason(const std::string& finish_reason, bool has_tool_calls);

void write_anthropic_error(httplib::Response& resp, int status,
                           const std::string& type, const std::string& message);

void handle_anthropic_messages(const httplib::Request& req, httplib::Response& resp,
                               const GatewayDeps& deps);

void handle_anthropic_count_tokens(const httplib::Request& req, httplib::Response& resp,
                                   const GatewayDeps& deps);

} // namespace inferdeck::gateway
