foundation::Result<std::vector<std::byte>> decode_base64(
    std::string_view encoded) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> values{};
    values.fill(-1);
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        values[static_cast<unsigned char>(alphabet[index])] =
            static_cast<int>(index);
    }
    if (encoded.empty() || encoded.size() % 4 != 0) {
        return foundation::Err<std::vector<std::byte>>(
            foundation::ErrorCode::InvalidArgument,
            "image_url contains invalid base64 length");
    }
    std::size_t padding = 0;
    if (encoded.ends_with("==")) padding = 2;
    else if (encoded.ends_with('=')) padding = 1;
    const std::size_t data_size = encoded.size() - padding;
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const unsigned char character =
            static_cast<unsigned char>(encoded[index]);
        if (index >= data_size) {
            if (character != '=') {
                return foundation::Err<std::vector<std::byte>>(
                    foundation::ErrorCode::InvalidArgument,
                    "image_url contains invalid base64 padding");
            }
        } else if (values[character] < 0) {
            return foundation::Err<std::vector<std::byte>>(
                foundation::ErrorCode::InvalidArgument,
                "image_url contains invalid base64 data");
        }
    }
    std::vector<std::byte> output;
    output.reserve(encoded.size() / 4 * 3 - padding);
    for (std::size_t index = 0; index < encoded.size(); index += 4) {
        const int first = values[static_cast<unsigned char>(encoded[index])];
        const int second = values[static_cast<unsigned char>(encoded[index + 1])];
        const int third = encoded[index + 2] == '=' ? 0
            : values[static_cast<unsigned char>(encoded[index + 2])];
        const int fourth = encoded[index + 3] == '=' ? 0
            : values[static_cast<unsigned char>(encoded[index + 3])];
        if (first < 0 || second < 0 || third < 0 || fourth < 0) {
            return foundation::Err<std::vector<std::byte>>(
                foundation::ErrorCode::InvalidArgument,
                "image_url contains invalid base64 data");
        }
        const std::uint32_t value =
            (static_cast<std::uint32_t>(first) << 18) |
            (static_cast<std::uint32_t>(second) << 12) |
            (static_cast<std::uint32_t>(third) << 6) |
            static_cast<std::uint32_t>(fourth);
        output.push_back(static_cast<std::byte>((value >> 16) & 0xff));
        if (encoded[index + 2] != '=') {
            output.push_back(static_cast<std::byte>((value >> 8) & 0xff));
        }
        if (encoded[index + 3] != '=') {
            output.push_back(static_cast<std::byte>(value & 0xff));
        }
    }
    if ((padding == 1 &&
         (values[static_cast<unsigned char>(encoded[data_size - 1])] & 0x3) != 0) ||
        (padding == 2 &&
         (values[static_cast<unsigned char>(encoded[data_size - 1])] & 0xf) != 0)) {
        return foundation::Err<std::vector<std::byte>>(
            foundation::ErrorCode::InvalidArgument,
            "image_url contains non-canonical base64 padding");
    }
    if (output.empty()) {
        return foundation::Err<std::vector<std::byte>>(
            foundation::ErrorCode::InvalidArgument,
            "image_url decoded to empty data");
    }
    return foundation::Ok(std::move(output));
}

foundation::Result<inference::Content> parse_image(
    const nlohmann::json& part) {
    if (!part.contains("image_url")) {
        return foundation::Err<inference::Content>(
            foundation::ErrorCode::InvalidArgument,
            "image content requires image_url");
    }
    const auto& value = part["image_url"];
    std::string url;
    std::string detail;
    if (value.is_string()) {
        url = value.get<std::string>();
        if (part.contains("detail") && part["detail"].is_string()) {
            detail = part["detail"].get<std::string>();
        }
    } else if (value.is_object() && value.contains("url") &&
               value["url"].is_string()) {
        url = value["url"].get<std::string>();
        if (value.contains("detail") && value["detail"].is_string()) {
            detail = value["detail"].get<std::string>();
        }
    } else {
        return foundation::Err<inference::Content>(
            foundation::ErrorCode::InvalidArgument,
            "image_url must be a string or object with string url");
    }
    const auto comma = url.find(',');
    if (comma == std::string::npos || !url.starts_with("data:image/") ||
        url.substr(0, comma).find(";base64") == std::string::npos) {
        return foundation::Err<inference::Content>(
            foundation::ErrorCode::InvalidArgument,
            "image_url must be a base64 data:image URL");
    }
    if (url.size() - comma - 1 > 32U * 1024U * 1024U) {
        return foundation::Err<inference::Content>(
            foundation::ErrorCode::InvalidArgument,
            "image payload exceeds 32 MiB encoded");
    }
    auto bytes = decode_base64(std::string_view(url).substr(comma + 1));
    if (!bytes) {
        return foundation::Err<inference::Content>(bytes.error().code,
                                                   bytes.error().message);
    }
    inference::ImageContent content;
    content.bytes = std::move(*bytes);
    content.media_type = url.substr(5, url.find(';') - 5);
    content.detail = std::move(detail);
    return foundation::Ok(inference::Content{std::move(content)});
}

foundation::Result<inference::Message> parse_message(
    const nlohmann::json& value, bool allow_extensions,
    bool responses_content_types) {
    if (!value.is_object() || !value.contains("role") ||
        !value["role"].is_string()) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "messages entries require a string role");
    }
    const std::string role = value["role"].get<std::string>();
    if (role != "developer" && role != "system" && role != "user" &&
        role != "assistant" && role != "tool" && role != "function") {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "messages role is unsupported");
    }
    static const std::unordered_set<std::string> strict_fields{
        "role", "content", "name", "tool_call_id", "tool_calls", "audio",
        "function_call", "refusal", "phase",
    };
    if (!allow_extensions) {
        auto fields = require_fields(value, strict_fields, "message");
        if (!fields) {
            return foundation::Err<inference::Message>(
                fields.error().code, fields.error().message);
        }
    }
    inference::Message message;
    message.role = inference::Message::parse_role(role);
    if (value.contains("name") && !value["name"].is_string()) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "message name must be a string");
    }
    if (value.contains("name")) {
        message.name = value["name"].get<std::string>();
    }
    if (value.contains("tool_call_id") &&
        !value["tool_call_id"].is_string()) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "message tool_call_id must be a string");
    }
    if (value.contains("tool_call_id")) {
        message.tool_call_id = value["tool_call_id"].get<std::string>();
    }
    if (role == "function" &&
        (!value.contains("name") || !value["name"].is_string() ||
         value["name"].get_ref<const std::string&>().empty())) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "function messages require a non-empty name");
    }
    if (value.contains("audio") && !value["audio"].is_null() &&
        (!value["audio"].is_object() ||
         !value["audio"].contains("id") ||
         !value["audio"]["id"].is_string())) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "assistant message audio requires a string id");
    }
    if (value.contains("refusal") && !value["refusal"].is_null() &&
        !value["refusal"].is_string()) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "assistant message refusal must be a string");
    }
    if (value.contains("phase") && !value["phase"].is_null() &&
        (!value["phase"].is_string() ||
         (value["phase"] != "commentary" &&
          value["phase"] != "final_answer"))) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "assistant message phase must be commentary or final_answer");
    }
    if (allow_extensions && value.contains("reasoning_content") &&
        value["reasoning_content"].is_string()) {
        message.reasoning = value["reasoning_content"].get<std::string>();
    }
    if (value.contains("content") && value["content"].is_string()) {
        message.content.emplace_back(inference::TextContent{
            value["content"].get<std::string>()});
    } else if (value.contains("content") && value["content"].is_array()) {
        for (const auto& part : value["content"]) {
            if (!part.is_object() || !part.contains("type") ||
                !part["type"].is_string()) {
                return foundation::Err<inference::Message>(
                    foundation::ErrorCode::InvalidArgument,
                    "message content parts require a string type");
            }
            const std::string type = part["type"].get<std::string>();
            if (part.contains("prompt_cache_breakpoint") &&
                !part["prompt_cache_breakpoint"].is_null()) {
                const auto& breakpoint = part["prompt_cache_breakpoint"];
                if (!breakpoint.is_object() || breakpoint.size() != 1 ||
                    !breakpoint.contains("mode") ||
                    breakpoint["mode"] != "explicit") {
                    return foundation::Err<inference::Message>(
                        foundation::ErrorCode::InvalidArgument,
                        "prompt_cache_breakpoint.mode must be explicit");
                }
            }
            const bool text_type = allow_extensions
                ? (type == "text" || type == "input_text")
                : (responses_content_types ?
                       (type == "input_text" || type == "output_text")
                                           : type == "text");
            const bool image_type = allow_extensions
                ? (type == "image" || type == "image_url" ||
                   type == "input_image")
                : (responses_content_types ? type == "input_image"
                                           : type == "image_url");
            if (text_type) {
                if (!allow_extensions) {
                    const std::unordered_set<std::string> fields =
                        type == "output_text"
                        ? std::unordered_set<std::string>{
                              "type", "text", "annotations", "logprobs"}
                        : std::unordered_set<std::string>{
                              "type", "text", "prompt_cache_breakpoint"};
                    auto allowed = require_fields(
                        part, fields, "message text content");
                    if (!allowed) {
                        return foundation::Err<inference::Message>(
                            allowed.error().code, allowed.error().message);
                    }
                }
                if (!part.contains("text") || !part["text"].is_string()) {
                    return foundation::Err<inference::Message>(
                        foundation::ErrorCode::InvalidArgument,
                        "text content requires string text");
                }
                message.content.emplace_back(inference::TextContent{
                    part["text"].get<std::string>()});
            } else if (image_type) {
                if (!allow_extensions) {
                    const std::unordered_set<std::string> fields =
                        responses_content_types
                        ? std::unordered_set<std::string>{
                              "type", "image_url", "file_id", "detail",
                              "prompt_cache_breakpoint"}
                        : std::unordered_set<std::string>{
                              "type", "image_url",
                              "prompt_cache_breakpoint"};
                    auto allowed = require_fields(
                        part, fields, "message image content");
                    if (!allowed) {
                        return foundation::Err<inference::Message>(
                            allowed.error().code, allowed.error().message);
                    }
                }
                if (responses_content_types) {
                    if (part.contains("detail") && !part["detail"].is_null() &&
                        (!part["detail"].is_string() ||
                         (part["detail"] != "auto" &&
                          part["detail"] != "low" &&
                          part["detail"] != "high" &&
                          part["detail"] != "original"))) {
                        return foundation::Err<inference::Message>(
                            foundation::ErrorCode::InvalidArgument,
                            "input_image detail is invalid");
                    }
                    bool source = false;
                    for (const auto field : {"image_url", "file_id"}) {
                        if (!part.contains(field) || part[field].is_null()) continue;
                        if (!part[field].is_string() ||
                            part[field].get_ref<const std::string&>().empty()) {
                            return foundation::Err<inference::Message>(
                                foundation::ErrorCode::InvalidArgument,
                                std::string(field) +
                                    " must be a non-empty string");
                        }
                        source = true;
                    }
                    if (!source) {
                        return foundation::Err<inference::Message>(
                            foundation::ErrorCode::InvalidArgument,
                            "input_image requires image_url or file_id");
                    }
                }
                const bool local_image = part.contains("image_url") &&
                    part["image_url"].is_string() &&
                    part["image_url"].get_ref<const std::string&>().starts_with(
                        "data:image/");
                if (local_image || !responses_content_types) {
                    auto content = parse_image(part);
                    if (!content) {
                        return foundation::Err<inference::Message>(
                            content.error().code, content.error().message);
                    }
                    message.content.push_back(std::move(*content));
                }
            } else if (type == "refusal") {
                static const std::unordered_set<std::string> fields{
                    "type", "refusal",
                };
                auto allowed = require_fields(
                    part, fields, "message refusal content");
                if (!allowed || !part.contains("refusal") ||
                    !part["refusal"].is_string()) {
                    return foundation::Err<inference::Message>(
                        foundation::ErrorCode::InvalidArgument,
                        "refusal content requires string refusal");
                }
                message.content.emplace_back(inference::TextContent{
                    part["refusal"].get<std::string>()});
            } else if (responses_content_types && type == "input_file") {
                static const std::unordered_set<std::string> fields{
                    "type", "detail", "file_data", "file_id", "file_url",
                    "filename", "prompt_cache_breakpoint",
                };
                auto allowed = require_fields(part, fields, "input file");
                if (!allowed) {
                    return foundation::Err<inference::Message>(
                        allowed.error().code, allowed.error().message);
                }
                bool source = false;
                for (const auto field : {"file_data", "file_id", "file_url"}) {
                    if (!part.contains(field) || part[field].is_null()) continue;
                    if (!part[field].is_string() ||
                        part[field].get_ref<const std::string&>().empty()) {
                        return foundation::Err<inference::Message>(
                            foundation::ErrorCode::InvalidArgument,
                            std::string(field) + " must be a non-empty string");
                    }
                    source = true;
                }
                if (!source) {
                    return foundation::Err<inference::Message>(
                        foundation::ErrorCode::InvalidArgument,
                        "input_file requires file_data, file_id, or file_url");
                }
                if (part.contains("detail") && !part["detail"].is_null() &&
                    (!part["detail"].is_string() ||
                     (part["detail"] != "auto" && part["detail"] != "low" &&
                      part["detail"] != "high"))) {
                    return foundation::Err<inference::Message>(
                        foundation::ErrorCode::InvalidArgument,
                        "input_file detail must be auto, low, or high");
                }
            } else if (!responses_content_types &&
                       (type == "input_audio" || type == "file")) {
                if (type == "input_audio" &&
                    (!part.contains("input_audio") ||
                     !part["input_audio"].is_object() ||
                     !part["input_audio"].contains("data") ||
                     !part["input_audio"]["data"].is_string() ||
                     !part["input_audio"].contains("format") ||
                     !part["input_audio"]["format"].is_string())) {
                    return foundation::Err<inference::Message>(
                        foundation::ErrorCode::InvalidArgument,
                        "input_audio requires string data and format");
                }
                if (type == "file" &&
                    (!part.contains("file") || !part["file"].is_object())) {
                    return foundation::Err<inference::Message>(
                        foundation::ErrorCode::InvalidArgument,
                        "file content requires a file object");
                }
            } else {
                return foundation::Err<inference::Message>(
                    foundation::ErrorCode::InvalidArgument,
                    "message content type is unsupported: " + type);
            }
        }
    } else if (value.contains("content") && !value["content"].is_null()) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "message content must be a string, array, or null");
    }
    if (!value.contains("content") &&
        !(role == "assistant" &&
          (value.contains("tool_calls") ||
           value.contains("function_call")))) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "message content is required");
    }
    if (role == "tool" && message.tool_call_id.empty()) {
        return foundation::Err<inference::Message>(
            foundation::ErrorCode::InvalidArgument,
            "tool messages require a non-empty tool_call_id");
    }
    if (value.contains("tool_calls")) {
        if (!value["tool_calls"].is_array()) {
            return foundation::Err<inference::Message>(
                foundation::ErrorCode::InvalidArgument,
                "message tool_calls must be an array");
        }
        for (const auto& item : value["tool_calls"]) {
            static const std::unordered_set<std::string> item_fields{
                "id", "type", "function",
            };
            if (!item.is_object() || !item.contains("type") ||
                !item["type"].is_string() ||
                item["type"].get<std::string>() != "function" ||
                !item.contains("function") ||
                !item["function"].is_object()) {
                return foundation::Err<inference::Message>(
                    foundation::ErrorCode::InvalidArgument,
                    "message tool_calls entries require type function");
            }
            if (!allow_extensions) {
                auto allowed = require_fields(
                    item, item_fields, "message tool call");
                if (!allowed) {
                    return foundation::Err<inference::Message>(
                        allowed.error().code, allowed.error().message);
                }
            }
            const auto& function = item["function"];
            static const std::unordered_set<std::string> function_fields{
                "name", "arguments",
            };
            if (!allow_extensions) {
                auto allowed = require_fields(
                    function, function_fields, "message tool call function");
                if (!allowed) {
                    return foundation::Err<inference::Message>(
                        allowed.error().code, allowed.error().message);
                }
            }
            if (!item.contains("id") || !item["id"].is_string() ||
                item["id"].get_ref<const std::string&>().empty() ||
                !function.contains("name") || !function["name"].is_string() ||
                function["name"].get_ref<const std::string&>().empty() ||
                !function.contains("arguments") ||
                (!allow_extensions && !function["arguments"].is_string())) {
                return foundation::Err<inference::Message>(
                    foundation::ErrorCode::InvalidArgument,
                    "message tool call requires non-empty string id and name and string arguments");
            }
            inference::FunctionCall call;
            if (item.contains("id") && item["id"].is_string()) {
                call.id = item["id"].get<std::string>();
            }
            call.name = function["name"].get<std::string>();
            call.arguments = function["arguments"].is_string()
                ? function["arguments"].get<std::string>()
                : function["arguments"].dump();
            message.tool_calls.push_back(std::move(call));
        }
    }
    if (value.contains("function_call") &&
        !value["function_call"].is_null()) {
        if (value.contains("tool_calls")) {
            return foundation::Err<inference::Message>(
                foundation::ErrorCode::InvalidArgument,
                "assistant message function_call and tool_calls are mutually exclusive");
        }
        const auto& function = value["function_call"];
        if (!function.is_object() || function.size() != 2 ||
            !function.contains("name") || !function["name"].is_string() ||
            function["name"].get_ref<const std::string&>().empty() ||
            !function.contains("arguments") ||
            !function["arguments"].is_string()) {
            return foundation::Err<inference::Message>(
                foundation::ErrorCode::InvalidArgument,
                "assistant message function_call requires string name and arguments");
        }
        inference::FunctionCall call;
        call.name = function["name"].get<std::string>();
        call.arguments = function["arguments"].get<std::string>();
        message.tool_calls.push_back(std::move(call));
    }
    if (message.content.empty() && value.contains("refusal") &&
        value["refusal"].is_string()) {
        message.content.emplace_back(inference::TextContent{
            value["refusal"].get<std::string>()});
    }
    return foundation::Ok(std::move(message));
}

inference::ReasoningFormat reasoning_format(const std::string& value) {
    if (value == "none") return inference::ReasoningFormat::None;
    if (value == "deepseek") return inference::ReasoningFormat::DeepSeek;
    if (value == "deepseek_legacy" || value == "deepseek-legacy") {
        return inference::ReasoningFormat::DeepSeekLegacy;
    }
    return inference::ReasoningFormat::Automatic;
}

}

foundation::Result<inference::Message> parse_openai_message(
    const nlohmann::json& value, bool allow_extensions) {
    return parse_message(value, allow_extensions, true);
}
