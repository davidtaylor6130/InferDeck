#include "messaging/conversions.hpp"

#include <string>

#include "foundation/result.hpp"

using namespace inferdeck::messaging;
using inferdeck::foundation::Err;
using inferdeck::foundation::ErrorCode;
using inferdeck::foundation::Ok;
using inferdeck::foundation::Result;

namespace {

std::string get_str(const Json& j, const char* key) {
    if (!j.contains(key)) return {};
    const auto& v = j.at(key);
    return v.is_string() ? v.get<std::string>() : std::string{};
}

std::optional<std::string> opt_str(const Json& j, const char* key) {
    if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
    const auto& v = j.at(key);
    if (v.is_string()) return v.get<std::string>();
    return std::nullopt;
}

std::optional<double> opt_num(const Json& j, const char* key) {
    if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
    const auto& v = j.at(key);
    if (v.is_number()) return v.get<double>();
    return std::nullopt;
}

std::optional<int> opt_int(const Json& j, const char* key) {
    if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
    const auto& v = j.at(key);
    if (v.is_number()) return v.get<int>();
    return std::nullopt;
}

std::optional<bool> opt_bool(const Json& j, const char* key) {
    if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
    const auto& v = j.at(key);
    if (v.is_boolean()) return v.get<bool>();
    return std::nullopt;
}

} // namespace

Json inferdeck::messaging::content_to_oai(const Content& c) {
    return std::visit([](const auto& x) -> Json {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, TextContent>) {
            return Json{{"type", "text"}, {"text", x.text}};
        } else if constexpr (std::is_same_v<T, ImageContent>) {
            Json obj{
                {"type", "image_url"},
                {"image_url", {
                    {"url", x.source == ImageContent::Source::Url
                              ? x.data
                              : "data:" + x.mime_type + ";base64," + x.data},
                    {"mime_type", x.mime_type}
                }}
            };
            if (x.detail) obj["image_url"]["detail"] = *x.detail;
            return obj;
        } else if constexpr (std::is_same_v<T, AudioContent>) {
            return Json{
                {"type", "input_audio"},
                {"input_audio", {
                    {"data", x.data},
                    {"format", x.format}
                }}
            };
        } else if constexpr (std::is_same_v<T, ToolCallContent>) {
            Json func{
                {"name", x.function.name},
                {"arguments", x.function.arguments}
            };
            Json obj{
                {"id", x.id},
                {"type", x.type},
                {"function", func}
            };
            return obj;
        } else if constexpr (std::is_same_v<T, ToolResultContent>) {
            return Json{
                {"type", "tool_result"},
                {"tool_call_id", x.tool_call_id},
                {"content", x.content},
                {"is_error", x.is_error}
            };
        } else if constexpr (std::is_same_v<T, ReasoningContent>) {
            return Json{{"type", "reasoning"}, {"reasoning", x.text}};
        } else if constexpr (std::is_same_v<T, DeveloperContent>) {
            return Json{{"type", "developer"}, {"developer", x.text}};
        }
        return Json{{"type", "text"}, {"text", ""}};
    }, c);
}

Result<Content> inferdeck::messaging::content_from_oai(const Json& j) {
    std::string type = j.value("type", std::string("text"));

    if (type == "text") {
        TextContent t;
        t.text = j.value("text", std::string{});
        return Ok<Content>(t);
    }
    if (type == "image_url") {
        ImageContent img;
        if (j.contains("image_url")) {
            const auto& u = j["image_url"];
            if (u.is_object()) {
                std::string url = u.value("url", std::string{});
                const std::string prefix = "data:";
                if (url.substr(0, prefix.size()) == prefix) {
                    auto semi = url.find(';');
                    auto comma = url.find(',');
                    if (semi != std::string::npos && comma != std::string::npos && semi < comma) {
                        std::string meta = url.substr(prefix.size(), semi - prefix.size());
                        auto slash = meta.find('/');
                        if (slash != std::string::npos) {
                            img.mime_type = meta;
                        }
                    }
                    img.source = ImageContent::Source::Base64;
                    img.data = url.substr(comma + 1);
                } else {
                    img.source = ImageContent::Source::Url;
                    img.data = url;
                }
                if (u.contains("detail") && u["detail"].is_string()) {
                    img.detail = u["detail"].get<std::string>();
                }
            }
        }
        return Ok<Content>(img);
    }
    if (type == "input_audio") {
        AudioContent a;
        if (j.contains("input_audio") && j["input_audio"].is_object()) {
            const auto& x = j["input_audio"];
            a.data = x.value("data", std::string{});
            a.format = x.value("format", std::string{"wav"});
        }
        return Ok<Content>(a);
    }
    if (type == "tool_call" || type == "function") {
        ToolCallContent tc;
        tc.id = j.value("id", std::string{});
        tc.type = j.value("type", std::string{"function"});
        if (j.contains("function") && j["function"].is_object()) {
            tc.function.name = j["function"].value("name", std::string{});
            tc.function.arguments = j["function"].value("arguments", std::string{});
        }
        return Ok<Content>(tc);
    }
    if (type == "tool_result") {
        ToolResultContent tr;
        tr.tool_call_id = j.value("tool_call_id", std::string{});
        tr.content = j.value("content", std::string{});
        tr.is_error = j.value("is_error", false);
        return Ok<Content>(tr);
    }
    if (type == "reasoning") {
        ReasoningContent r;
        r.text = j.value("reasoning", j.value("text", std::string{}));
        return Ok<Content>(r);
    }
    if (type == "developer") {
        DeveloperContent d;
        d.text = j.value("developer", j.value("text", std::string{}));
        return Ok<Content>(d);
    }
    return Err<Content>(ErrorCode::ParseError, "unknown content type: " + type);
}

Json inferdeck::messaging::message_to_oai(const Message& m) {
    Json obj;
    obj["role"] = messaging::to_string(m.role);
    if (!m.name.empty()) obj["name"] = m.name;
    if (m.tool_call_id) obj["tool_call_id"] = *m.tool_call_id;

    if (m.role == Role::Tool && m.content.size() == 1) {
        const Content& c = m.content.front();
        if (auto* tr = try_as<ToolResultContent>(c)) {
            obj["content"] = tr->content;
            return obj;
        }
    }

    if (m.content.size() == 1) {
        if (auto* tc = try_as<ToolCallContent>(m.content.front())) {
            Json func = {
                {"name", tc->function.name},
                {"arguments", tc->function.arguments}
            };
            obj["tool_calls"] = Json::array();
            obj["tool_calls"].push_back({
                {"id", tc->id},
                {"type", tc->type},
                {"function", func}
            });
            return obj;
        }
    }

    if (m.content.empty()) {
        obj["content"] = "";
    } else if (m.content.size() == 1) {
        obj["content"] = content_to_oai(m.content.front());
    } else {
        Json arr = Json::array();
        for (const auto& c : m.content) arr.push_back(content_to_oai(c));
        obj["content"] = arr;
    }
    return obj;
}

Result<Message> inferdeck::messaging::message_from_oai(const Json& j) {
    std::string role_str = j.value("role", std::string("user"));
    auto role_opt = role_from_string(role_str);
    if (!role_opt) {
        return Err<Message>(ErrorCode::ParseError, "invalid role: " + role_str);
    }

    Message m;
    m.role = *role_opt;
    m.name = j.value("name", std::string{});
    m.tool_call_id = opt_str(j, "tool_call_id");
    m.refusal = opt_str(j, "refusal");

    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        for (const auto& tc : j["tool_calls"]) {
            if (!tc.is_object()) continue;
            ToolCallContent tcc;
            tcc.id = tc.value("id", std::string{});
            tcc.type = tc.value("type", std::string{"function"});
            if (tc.contains("function") && tc["function"].is_object()) {
                tcc.function.name = tc["function"].value("name", std::string{});
                tcc.function.arguments = tc["function"].value("arguments", std::string{});
            }
            m.content.push_back(tcc);
        }
    }

    if (j.contains("content")) {
        const auto& c = j["content"];
        if (c.is_string()) {
            if (m.role == Role::Tool) {
                ToolResultContent tr;
                tr.tool_call_id = m.tool_call_id.value_or("");
                tr.content = c.get<std::string>();
                m.content.push_back(tr);
            } else {
                m.content.push_back(TextContent{c.get<std::string>()});
            }
        } else if (c.is_array()) {
            for (const auto& item : c) {
                auto parsed = content_from_oai(item);
                if (!parsed) {
                    return Err<Message>(ErrorCode::ParseError,
                                         "content parse: " + parsed.error().message);
                }
                m.content.push_back(*parsed);
            }
        } else if (c.is_object()) {
            auto parsed = content_from_oai(c);
            if (!parsed) {
                return Err<Message>(ErrorCode::ParseError,
                                     "content parse: " + parsed.error().message);
            }
            m.content.push_back(*parsed);
        }
    }

    return Ok(m);
}

Json inferdeck::messaging::conversation_to_oai(const Conversation& c) {
    Json obj;
    if (!c.model.empty()) obj["model"] = c.model;
    Json arr = Json::array();
    for (const auto& m : c.messages) arr.push_back(message_to_oai(m));
    obj["messages"] = arr;
    if (c.temperature) obj["temperature"] = *c.temperature;
    if (c.top_p)       obj["top_p"] = *c.top_p;
    if (c.top_k)       obj["top_k"] = *c.top_k;
    if (c.max_tokens)  obj["max_tokens"] = *c.max_tokens;
    if (c.stream)      obj["stream"] = *c.stream;
    if (c.thinking_mode) obj["thinking"] = *c.thinking_mode;
    if (c.reasoning_effort) obj["reasoning_effort"] = *c.reasoning_effort;
    return obj;
}

Result<Conversation> inferdeck::messaging::conversation_from_oai(const Json& j) {
    Conversation c;
    c.model = j.value("model", std::string{});
    c.temperature = opt_num(j, "temperature");
    c.top_p = opt_num(j, "top_p");
    c.top_k = opt_int(j, "top_k");
    c.max_tokens = opt_int(j, "max_tokens");
    c.stream = opt_bool(j, "stream");
    c.thinking_mode = opt_bool(j, "thinking");
    c.reasoning_effort = opt_str(j, "reasoning_effort");

    if (!j.contains("messages") || !j["messages"].is_array()) {
        return Err<Conversation>(ErrorCode::InvalidArgument, "missing 'messages' array");
    }
    for (const auto& mj : j["messages"]) {
        auto parsed = message_from_oai(mj);
        if (!parsed) {
            return Err<Conversation>(ErrorCode::ParseError,
                                      "message parse: " + parsed.error().message);
        }
        c.messages.push_back(std::move(*parsed));
    }
    return Ok(c);
}

Json inferdeck::messaging::content_to_anthropic(const Content& c) {
    return std::visit([](const auto& x) -> Json {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, TextContent>) {
            return Json{{"type", "text"}, {"text", x.text}};
        } else if constexpr (std::is_same_v<T, ImageContent>) {
            Json src = x.source == ImageContent::Source::Url
                ? Json{{"type", "url"}, {"url", x.data}}
                : Json{{"type", "base64"}, {"media_type", x.mime_type}, {"data", x.data}};
            return Json{{"type", "image"}, {"source", src}};
        } else if constexpr (std::is_same_v<T, ToolCallContent>) {
            Json input = Json::object();
            if (!x.function.arguments.empty()) {
                auto parsed = Json::parse(x.function.arguments, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object()) {
                    input = Json{x.function.arguments};
                } else {
                    input = std::move(parsed);
                }
            }
            return Json{
                {"type", "tool_use"},
                {"id", x.id},
                {"name", x.function.name},
                {"input", input}
            };
        } else if constexpr (std::is_same_v<T, ToolResultContent>) {
            return Json{
                {"type", "tool_result"},
                {"tool_use_id", x.tool_call_id},
                {"content", x.content},
                {"is_error", x.is_error}
            };
        } else {
            return Json{{"type", "text"}, {"text", ""}};
        }
    }, c);
}

Result<Content> inferdeck::messaging::content_from_anthropic(const Json& j) {
    std::string type = j.value("type", std::string("text"));
    if (type == "text") {
        TextContent t;
        t.text = j.value("text", std::string{});
        return Ok<Content>(t);
    }
    if (type == "image") {
        ImageContent img;
        if (j.contains("source") && j["source"].is_object()) {
            const auto& s = j["source"];
            std::string src = s.value("type", std::string{"base64"});
            if (src == "url") {
                img.source = ImageContent::Source::Url;
                img.data = s.value("url", std::string{});
            } else {
                img.source = ImageContent::Source::Base64;
                img.data = s.value("data", std::string{});
                img.mime_type = s.value("media_type", std::string{"image/png"});
            }
        }
        return Ok<Content>(img);
    }
    if (type == "tool_use") {
        ToolCallContent tc;
        tc.id = j.value("id", std::string{});
        tc.type = "function";
        tc.function.name = j.value("name", std::string{});
        if (j.contains("input")) {
            tc.function.arguments = j["input"].dump();
        }
        return Ok<Content>(tc);
    }
    if (type == "tool_result") {
        ToolResultContent tr;
        tr.tool_call_id = j.value("tool_use_id", std::string{});
        tr.is_error = j.value("is_error", false);
        if (j.contains("content")) {
            const auto& c = j["content"];
            if (c.is_string()) tr.content = c.get<std::string>();
            else tr.content = c.dump();
        }
        return Ok<Content>(tr);
    }
    return Err<Content>(ErrorCode::ParseError, "unknown anthropic content type: " + type);
}

Json inferdeck::messaging::message_to_anthropic(const Message& m) {
    Json obj;
    obj["role"] = messaging::to_string(m.role);
    if (m.role == Role::Assistant && !m.content.empty()) {
        for (const auto& c : m.content) {
            if (auto* tc = try_as<ToolCallContent>(c)) {
                Json j = content_to_anthropic(c);
                j["id"] = tc->id;
                j["name"] = tc->function.name;
                j["input"] = tc->function.arguments;
                if (!obj.contains("content")) obj["content"] = Json::array();
                obj["content"].push_back(j);
            } else {
                if (!obj.contains("content")) obj["content"] = Json::array();
                obj["content"].push_back(content_to_anthropic(c));
            }
        }
    } else {
        Json arr = Json::array();
        for (const auto& c : m.content) arr.push_back(content_to_anthropic(c));
        obj["content"] = arr;
    }
    return obj;
}

Result<Message> inferdeck::messaging::message_from_anthropic(const Json& j) {
    auto role_opt = role_from_string(j.value("role", std::string("user")));
    if (!role_opt) {
        return Err<Message>(ErrorCode::ParseError, "invalid anthropic role");
    }
    Message m;
    m.role = *role_opt;

    if (j.contains("content")) {
        const auto& c = j["content"];
        if (c.is_string()) {
            m.content.push_back(TextContent{c.get<std::string>()});
        } else if (c.is_array()) {
            for (const auto& item : c) {
                auto parsed = content_from_anthropic(item);
                if (!parsed) {
                    return Err<Message>(ErrorCode::ParseError,
                                         "anthropic content: " + parsed.error().message);
                }
                m.content.push_back(*parsed);
            }
        }
    }
    return Ok(m);
}

Json inferdeck::messaging::content_to_internal(const Content& c) {
    return content_to_oai(c);
}

Result<Content> inferdeck::messaging::content_from_internal(const Json& j) {
    return content_from_oai(j);
}

Json inferdeck::messaging::message_to_internal(const Message& m) {
    return message_to_oai(m);
}

Result<Message> inferdeck::messaging::message_from_internal(const Json& j) {
    return message_from_oai(j);
}
