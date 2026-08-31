bool integer_in_range(const nlohmann::json& value, std::int64_t minimum,
                      std::int64_t maximum) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        return number <= static_cast<std::uint64_t>(maximum) &&
               (minimum <= 0 ||
                number >= static_cast<std::uint64_t>(minimum));
    }
    if (!value.is_number_integer()) return false;
    const auto number = value.get<std::int64_t>();
    return number >= minimum && number <= maximum;
}

foundation::Result<void> append_message(
    model::InferenceRequest& request, const nlohmann::json& value) {
    auto message = parse_openai_message(value);
    if (!message) {
        return foundation::Err<void>(
            message.error().code, message.error().message);
    }
    request.messages.push_back(std::move(*message));
    return foundation::Ok();
}

foundation::Result<void> parse_input_item(
    model::InferenceRequest& request, const nlohmann::json& item,
    std::optional<std::string>& unsupported_type) {
    if (!item.is_object()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "each input item must be an object");
    }
    if (item.contains("type") && !item["type"].is_string()) {
        return foundation::Err<void>(
            foundation::ErrorCode::InvalidArgument,
            "input item type must be a string");
    }
    const std::string type = item.value("type", "message");
    if (type == "message") {
        static const std::unordered_set<std::string> fields{
            "type", "role", "content", "id", "status", "phase",
        };
        auto allowed = require_fields(item, fields, "message");
        if (!allowed) return allowed;
        if (!item.contains("role") || !item["role"].is_string() ||
            !item.contains("content")) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "message requires string role and content");
        }
        const auto role = item["role"].get<std::string>();
        if (role != "developer" && role != "system" && role != "user" &&
            role != "assistant") {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "unsupported message role: " + role);
        }
        if (item.contains("id") && !item["id"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "message id must be a string");
        }
        if (item.contains("status") && !item["status"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "message status must be a string");
        }
        auto message = item;
        message.erase("type");
        message.erase("id");
        message.erase("status");
        return append_message(request, message);
    }
    if (type == "function_call") {
        static const std::unordered_set<std::string> fields{
            "type", "call_id", "id", "name", "arguments", "status",
        };
        auto allowed = require_fields(item, fields, "function_call");
        if (!allowed) return allowed;
        if (!item.contains("name") || !item["name"].is_string() ||
            item["name"].get_ref<const std::string&>().empty() ||
            !item.contains("arguments") ||
            !item["arguments"].is_string()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "function_call requires string name and arguments");
        }
        std::string call_id;
        if (item.contains("call_id") && item["call_id"].is_string()) {
            call_id = item["call_id"].get<std::string>();
        } else if (item.contains("id") && item["id"].is_string()) {
            call_id = item["id"].get<std::string>();
        }
        if (call_id.empty()) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "function_call requires non-empty call_id");
        }
        inference::Message message;
        message.role = inference::MessageRole::Assistant;
        message.content.emplace_back(inference::TextContent{});
        message.tool_calls.push_back(inference::FunctionCall{
            std::move(call_id),
            item["name"].get<std::string>(),
            item["arguments"].get<std::string>(),
        });
        request.messages.push_back(std::move(message));
        return foundation::Ok();
    }
    if (type == "function_call_output") {
        static const std::unordered_set<std::string> fields{
            "type", "call_id", "output", "id", "status",
        };
        auto allowed = require_fields(item, fields, "function_call_output");
        if (!allowed) return allowed;
        if (!item.contains("call_id") || !item["call_id"].is_string() ||
            item["call_id"].get_ref<const std::string&>().empty() ||
            !item.contains("output") ||
            !(item["output"].is_string() || item["output"].is_array())) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "function_call_output requires call_id and string or content-array output");
        }
        return append_message(request, nlohmann::json{
            {"role", "tool"},
            {"tool_call_id", item["call_id"]},
            {"content", item["output"]},
        });
    }
    static const std::unordered_set<std::string> official_service_items{
        "additional_tools", "apply_patch_call", "apply_patch_call_output",
        "code_interpreter_call", "compaction", "compaction_trigger",
        "computer_call", "computer_call_output", "custom_tool_call",
        "custom_tool_call_output", "file_search_call",
        "image_generation_call", "item_reference", "local_shell_call",
        "local_shell_call_output", "mcp_approval_request",
        "mcp_approval_response", "mcp_call", "mcp_list_tools", "program",
        "program_output", "reasoning", "shell_call", "shell_call_output",
        "tool_search_call", "tool_search_output", "web_search_call",
    };
    if (official_service_items.contains(type)) {
        if (!unsupported_type) unsupported_type = type;
        return foundation::Ok();
    }
    return foundation::Err<void>(
        foundation::ErrorCode::InvalidArgument,
        "unsupported input item type: " + type);
}

std::optional<std::pair<std::string, std::string>> input_capability(
    const nlohmann::json& value) {
    if (value.is_array()) {
        for (const auto& item : value) {
            if (auto capability = input_capability(item)) return capability;
        }
        return std::nullopt;
    }
    if (!value.is_object()) return std::nullopt;
    if (value.contains("prompt_cache_breakpoint") &&
        !value["prompt_cache_breakpoint"].is_null()) {
        return std::pair{"prompt_cache_options",
                         "explicit prompt-cache breakpoints"};
    }
    const auto type = value.contains("type") && value["type"].is_string()
        ? value["type"].get<std::string>() : std::string{};
    if (type == "input_file") {
        return std::pair{"input", "file input"};
    }
    if (type == "input_image") {
        if (value.contains("file_id") && !value["file_id"].is_null()) {
            return std::pair{"input", "hosted image input"};
        }
        if (value.contains("image_url") && value["image_url"].is_string() &&
            !value["image_url"].get_ref<const std::string&>().starts_with(
                "data:image/")) {
            return std::pair{"input", "remote image input"};
        }
    }
    if (value.contains("content")) {
        return input_capability(value["content"]);
    }
    return std::nullopt;
}
