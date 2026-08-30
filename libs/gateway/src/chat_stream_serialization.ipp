std::string dump_json(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string sse_chunk_json(const std::string& id, const std::string& model,
                           std::int64_t created, const nlohmann::json& delta,
                           const nlohmann::json& logprobs,
                           bool include_usage,
                           const std::string& service_tier,
                           bool include_obfuscation) {
    nlohmann::json chunk = {
        {"id", id},
        {"object", "chat.completion.chunk"},
        {"created", created},
        {"model", model},
        {"choices", nlohmann::json::array({
            {
                {"index", 0},
                {"delta", delta},
                {"finish_reason", nullptr},
            }
        })},
    };
    if (!logprobs.is_null()) {
        chunk["choices"][0]["logprobs"] = logprobs;
    }
    if (!service_tier.empty()) chunk["service_tier"] = service_tier;
    if (include_obfuscation) chunk["obfuscation"] = make_id().substr(9);
    if (include_usage) chunk["usage"] = nullptr;
    return "data: " + dump_json(chunk) + "\n\n";
}

std::string sse_terminal(const std::string& id, const std::string& model,
                         std::int64_t created,
                         const std::string& finish_reason,
                         const model::InferenceResult* result,
                         bool include_usage,
                         const std::string& service_tier,
                         bool include_obfuscation) {
    nlohmann::json finish = {
        {"id", id},
        {"object", "chat.completion.chunk"},
        {"created", created},
        {"model", model},
        {"choices", nlohmann::json::array({
            {
                {"index", 0},
                {"delta", nlohmann::json::object()},
                {"finish_reason", finish_reason},
            }
        })},
    };
    if (!service_tier.empty()) finish["service_tier"] = service_tier;
    if (include_obfuscation) finish["obfuscation"] = make_id().substr(9);
    if (include_usage) finish["usage"] = nullptr;
    std::string output = "data: " + dump_json(finish) + "\n\n";
    if (include_usage && result) {
        nlohmann::json usage = {
            {"id", id},
            {"object", "chat.completion.chunk"},
            {"created", created},
            {"model", model},
            {"choices", nlohmann::json::array()},
            {"usage", {
            {"prompt_tokens", result->prompt_tokens},
            {"prompt_tokens_details", {{"cached_tokens", result->cached_prompt_tokens}}},
            {"completion_tokens", result->completion_tokens},
            {"total_tokens", result->prompt_tokens + result->completion_tokens},
            }},
        };
        if (!service_tier.empty()) usage["service_tier"] = service_tier;
        if (include_obfuscation) usage["obfuscation"] = make_id().substr(9);
        output += "data: " + dump_json(usage) + "\n\n";
    }
    output += "data: [DONE]\n\n";
    return output;
}

nlohmann::json tool_call_json(const model::ToolCall& tc) {
    nlohmann::json out = {
        {"type", tc.type.empty() ? "function" : tc.type},
        {"function", {
            {"name", tc.function_name},
            {"arguments", tc.function_arguments},
        }},
    };
    if (!tc.id.empty()) out["id"] = tc.id;
    return out;
}

nlohmann::json tool_call_delta_json(const model::ToolCallDelta& tc) {
    nlohmann::json out = {{"index", tc.index}};
    if (!tc.id.empty()) {
        out["id"] = tc.id;
        out["type"] = tc.type.empty() ? "function" : tc.type;
    }
    if (!tc.function_name.empty() || !tc.function_arguments.empty()) {
        nlohmann::json fn = nlohmann::json::object();
        if (!tc.function_name.empty()) fn["name"] = tc.function_name;
        if (!tc.function_arguments.empty()) fn["arguments"] = tc.function_arguments;
        out["function"] = fn;
    }
    return out;
}

nlohmann::json token_logprobs_json(
    const std::vector<inference::TokenLogprob>& values) {
    nlohmann::json content = nlohmann::json::array();
    for (const auto& value : values) {
        nlohmann::json top = nlohmann::json::array();
        for (const auto& candidate : value.top_logprobs) {
            top.push_back({
                {"token", candidate.token},
                {"logprob", candidate.logprob},
                {"bytes", candidate.bytes},
            });
        }
        content.push_back({
            {"token", value.token},
            {"logprob", value.logprob},
            {"bytes", value.bytes},
            {"top_logprobs", std::move(top)},
        });
    }
    return {
        {"content", std::move(content)},
        {"refusal", nullptr},
    };
}

foundation::Result<std::optional<std::string>> normalize_reasoning_request(
    nlohmann::json& body, const model::ModelInfo& info) {
    std::optional<std::string> requested;
    bool supplied = false;
    bool explicit_kwarg = false;
    if (body.contains("reasoning_effort")) {
        supplied = true;
        if (!body["reasoning_effort"].is_string()) {
            return foundation::Err<std::optional<std::string>>(
                foundation::ErrorCode::InvalidArgument,
                "reasoning_effort must be a string");
        }
        requested = body["reasoning_effort"].get<std::string>();
    }
    if (body.contains("chat_template_kwargs")) {
        if (!body["chat_template_kwargs"].is_object()) {
            return foundation::Err<std::optional<std::string>>(
                foundation::ErrorCode::InvalidArgument,
                "chat_template_kwargs must be an object");
        }
        auto& kwargs = body["chat_template_kwargs"];
        if (kwargs.contains("reasoning_effort")) {
            supplied = true;
            explicit_kwarg = true;
            if (!kwargs["reasoning_effort"].is_string()) {
                return foundation::Err<std::optional<std::string>>(
                    foundation::ErrorCode::InvalidArgument,
                    "chat_template_kwargs.reasoning_effort must be a string");
            }
            requested = kwargs["reasoning_effort"].get<std::string>();
        }
    }
    if (!requested && info.reasoning.supported &&
        !info.reasoning.default_effort.empty()) {
        requested = info.reasoning.default_effort;
    }
    if (!requested) return foundation::Ok(std::optional<std::string>{});
    if (!info.reasoning.supported) {
        return foundation::Err<std::optional<std::string>>(
            foundation::ErrorCode::InvalidArgument,
            "model does not support reasoning effort: " + info.name);
    }

    std::string resolved = *requested;
    if (const auto alias = info.reasoning.aliases.find(resolved);
        alias != info.reasoning.aliases.end()) {
        resolved = alias->second;
    }
    if (resolved == "none") {
        if (!info.reasoning.none_disables) {
            return foundation::Err<std::optional<std::string>>(
                foundation::ErrorCode::InvalidArgument,
                "reasoning effort 'none' is not supported by model: " + info.name);
        }
    } else if (std::find(info.reasoning.efforts.begin(),
                         info.reasoning.efforts.end(), resolved) ==
               info.reasoning.efforts.end()) {
        return foundation::Err<std::optional<std::string>>(
            foundation::ErrorCode::InvalidArgument,
            "unsupported reasoning effort '" + *requested + "' for model: " +
                info.name);
    }

    body["reasoning_effort"] = resolved;
    if (explicit_kwarg) {
        body["chat_template_kwargs"]["reasoning_effort"] = resolved;
    }
    LOG_INFO("reasoning_effort_resolved", "model={} effort={} source={}",
             info.name, resolved,
             explicit_kwarg ? "chat_template_kwargs"
                            : (supplied ? "protocol" : "model_default"));
    return foundation::Ok(std::optional<std::string>{std::move(resolved)});
}

nlohmann::json delta_json(const model::InferenceDelta& delta,
                          bool include_reasoning_content) {
    nlohmann::json out = nlohmann::json::object();
    if (include_reasoning_content && !delta.reasoning_text.empty()) {
        out["reasoning_content"] = delta.reasoning_text;
    }
    if (!delta.content.empty()) out["content"] = delta.content;
    if (!delta.tool_calls.empty()) {
        out["tool_calls"] = nlohmann::json::array();
        for (const auto& tc : delta.tool_calls) {
            out["tool_calls"].push_back(tool_call_delta_json(tc));
        }
    }
    if (!delta.logprobs.empty()) {
        out["logprobs"] = token_logprobs_json(delta.logprobs);
    }
    return out;
}

std::size_t delta_size(const model::InferenceDelta& delta) {
    std::size_t size = delta.content.size() + delta.reasoning_text.size();
    for (const auto& call : delta.tool_calls) {
        size += call.id.size() + call.type.size() + call.function_name.size() +
                call.function_arguments.size();
    }
    for (const auto& token : delta.logprobs) {
        size += token.token.size() + token.bytes.size();
        for (const auto& candidate : token.top_logprobs) {
            size += candidate.token.size() + candidate.bytes.size();
        }
    }
    return size;
}

bool content_uses_vision(const nlohmann::json& content) {
    if (!content.is_array()) return false;
    for (const auto& part : content) {
        if (!part.is_object()) continue;
        const auto type = part.value("type", "");
        if (type == "image" || type == "image_url" || type == "input_image") return true;
    }
    return false;
}

bool chat_uses_vision(const nlohmann::json& body) {
    if (!body.contains("messages") || !body["messages"].is_array()) return false;
    for (const auto& message : body["messages"]) {
        if (message.is_object() && message.contains("content") &&
            content_uses_vision(message["content"])) {
            return true;
        }
    }
    return false;
}

bool chat_uses_content_type(const nlohmann::json& body,
                            const std::string& type) {
    if (!body.contains("messages") || !body["messages"].is_array()) {
        return false;
    }
    for (const auto& message : body["messages"]) {
        if (!message.is_object()) continue;
        if (type == "audio_history" && message.contains("audio") &&
            !message["audio"].is_null()) {
            return true;
        }
        if (!message.contains("content") ||
            !message["content"].is_array()) {
            continue;
        }
        for (const auto& part : message["content"]) {
            if (!part.is_object()) continue;
            if (part.value("type", "") == type) return true;
            if (type == "prompt_cache_breakpoint" &&
                part.contains("prompt_cache_breakpoint")) {
                return true;
            }
        }
    }
    return false;
}

bool chat_uses_non_function_tools(const nlohmann::json& body) {
    if (body.contains("tools") && body["tools"].is_array()) {
        for (const auto& tool : body["tools"]) {
            if (tool.is_object() && tool.value("type", "") != "function") {
                return true;
            }
        }
    }
    if (body.contains("tool_choice") &&
        body["tool_choice"].is_object()) {
        const auto type = body["tool_choice"].value("type", "");
        return type == "custom" || type == "allowed_tools";
    }
    return false;
}

} // namespace

std::string serialize_chat_stream_delta(const std::string& id,
                                        const std::string& model,
                                        std::int64_t created,
                                        const nlohmann::json& delta,
                                        bool include_usage,
                                        bool include_reasoning_content,
                                        const std::string& service_tier,
                                        bool include_obfuscation) {
    auto filtered = delta;
    if (!include_reasoning_content) filtered.erase("reasoning_content");
    nlohmann::json logprobs = nullptr;
    if (filtered.contains("logprobs")) {
        logprobs = std::move(filtered["logprobs"]);
        filtered.erase("logprobs");
    }
    return sse_chunk_json(
        id, model, created, filtered, logprobs, include_usage, service_tier,
        include_obfuscation);
}

std::string serialize_chat_stream_terminal(const std::string& id,
                                           const std::string& model,
                                           std::int64_t created,
                                           const std::string& finish_reason,
                                           const model::InferenceResult* result,
                                           bool include_usage,
                                           const std::string& service_tier,
                                           bool include_obfuscation) {
    return sse_terminal(
        id, model, created, finish_reason, result, include_usage,
        service_tier, include_obfuscation);
}
