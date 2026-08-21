#include "gateway/openai_routes.hpp"
#include "gateway/generation_session.hpp"
#include "gateway/openai_adapter.hpp"
#include "gateway/openai_error.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace inferdeck::gateway {

namespace {

std::string make_id(const char* prefix) {
    static std::mutex mutex;
    static std::mt19937_64 random{std::random_device{}()};
    std::lock_guard lock(mutex);
    return std::string(prefix) + std::to_string(random());
}

std::string base64_floats(const std::vector<float>& values) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes;
    bytes.reserve(values.size() * sizeof(float));
    for (const float value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        bytes.push_back(static_cast<unsigned char>(bits & 0xff));
        bytes.push_back(static_cast<unsigned char>((bits >> 8) & 0xff));
        bytes.push_back(static_cast<unsigned char>((bits >> 16) & 0xff));
        bytes.push_back(static_cast<unsigned char>((bits >> 24) & 0xff));
    }
    const std::size_t size = bytes.size();
    std::string output;
    output.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t chunk = static_cast<std::uint32_t>(bytes[i]) << 16 |
            static_cast<std::uint32_t>(i + 1 < size ? bytes[i + 1] : 0) << 8 |
            static_cast<std::uint32_t>(i + 2 < size ? bytes[i + 2] : 0);
        output.push_back(alphabet[(chunk >> 18) & 0x3f]);
        output.push_back(alphabet[(chunk >> 12) & 0x3f]);
        output.push_back(i + 1 < size ? alphabet[(chunk >> 6) & 0x3f] : '=');
        output.push_back(i + 2 < size ? alphabet[chunk & 0x3f] : '=');
    }
    return output;
}

foundation::Result<int> acquire_for_request(
    const httplib::Request& req, const GatewayDeps& deps,
    const std::string& model_name, int priority) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::minutes{5};
    const std::function<bool()> cancelled = [&req] {
        return req.is_connection_closed();
    };
    model::AcquireSlotOptions options;
    options.timeout = std::chrono::minutes{5};
    options.priority = std::clamp(priority, -100, 100);
    options.cancelled = cancelled;
    options.prepare = [&deps, &model_name, deadline, cancelled] {
        auto loaded = ensure_model_loaded(
            deps, model_name, deadline, cancelled);
        if (loaded.ok) return foundation::Ok();
        return foundation::Err<void>(loaded.error_code, loaded.message);
    };
    return deps.coordinator.acquire_slot(model_name, options);
}

foundation::Result<void> require_fields(
    const nlohmann::json& value,
    const std::unordered_set<std::string>& supported,
    const std::string& context);

foundation::Result<nlohmann::json> response_content_to_chat(const nlohmann::json& content) {
    if (content.is_string()) return foundation::Ok(content);
    if (!content.is_array()) {
        return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                               "message content must be a string or array");
    }
    if (content.empty()) {
        return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                               "message content must not be empty");
    }
    nlohmann::json parts = nlohmann::json::array();
    for (const auto& part : content) {
        if (!part.is_object()) {
            return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                   "input content part must be an object");
        }
        if (!part.contains("type") || !part["type"].is_string()) {
            return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                   "input content part requires string type");
        }
        const std::string type = part["type"].get<std::string>();
        if (type == "input_text" || type == "output_text") {
            const std::unordered_set<std::string> fields =
                type == "output_text"
                    ? std::unordered_set<std::string>{
                          "type", "text", "annotations",
                      }
                    : std::unordered_set<std::string>{"type", "text"};
            auto allowed = require_fields(part, fields, type);
            if (!allowed) {
                return foundation::Err<nlohmann::json>(
                    allowed.error().code, allowed.error().message);
            }
            if (!part.contains("text") || !part["text"].is_string()) {
                return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                       type + " requires string text");
            }
            if (part.contains("annotations") &&
                !part["annotations"].is_array()) {
                return foundation::Err<nlohmann::json>(
                    foundation::ErrorCode::InvalidArgument,
                    "output_text annotations must be an array");
            }
            parts.push_back({{"type", "text"}, {"text", part["text"]}});
        } else if (type == "input_image") {
            static const std::unordered_set<std::string> fields = {
                "type", "image_url", "detail",
            };
            auto allowed = require_fields(part, fields, "input_image");
            if (!allowed) {
                return foundation::Err<nlohmann::json>(
                    allowed.error().code, allowed.error().message);
            }
            if (!part.contains("image_url") || !part["image_url"].is_string()) {
                return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                       "input_image requires string image_url");
            }
            const std::string url = part["image_url"].get<std::string>();
            if (url.empty()) {
                return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                       "input_image requires image_url");
            }
            nlohmann::json image_url = {{"url", url}};
            if (part.contains("detail")) {
                if (!part["detail"].is_string()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "input_image detail must be a string");
                }
                const std::string detail = part["detail"].get<std::string>();
                if (detail != "auto" && detail != "low" && detail != "high") {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "input_image detail must be auto, low, or high");
                }
                image_url["detail"] = detail;
            }
            parts.push_back({{"type", "image_url"}, {"image_url", std::move(image_url)}});
        } else {
            return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                   "unsupported input content type: " + type);
        }
    }
    return foundation::Ok(std::move(parts));
}

bool response_input_uses_vision(const nlohmann::json& input) {
    if (input.is_array()) {
        return std::any_of(input.begin(), input.end(), [](const auto& item) {
            return response_input_uses_vision(item);
        });
    }
    if (!input.is_object()) return false;
    const auto type = input.contains("type") && input["type"].is_string()
        ? input["type"].get<std::string>() : std::string{};
    if (type == "image" || type == "image_url" || type == "input_image") return true;
    return input.contains("content") &&
           response_input_uses_vision(input["content"]);
}

bool integer_in_range(const nlohmann::json& value, std::int64_t minimum,
                      std::int64_t maximum) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        const auto unsigned_minimum =
            minimum <= 0 ? std::uint64_t{0}
                         : static_cast<std::uint64_t>(minimum);
        return number >= unsigned_minimum &&
               number <= static_cast<std::uint64_t>(maximum);
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        return number >= minimum && number <= maximum;
    }
    return false;
}

foundation::Result<void> require_fields(
    const nlohmann::json& value,
    const std::unordered_set<std::string>& supported,
    const std::string& context) {
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        if (!supported.contains(iterator.key())) {
            return foundation::Err<void>(
                foundation::ErrorCode::InvalidArgument,
                "unsupported " + context + " parameter: " + iterator.key());
        }
    }
    return foundation::Ok();
}

foundation::Result<nlohmann::json> responses_to_chat(const nlohmann::json& body) {
    static const std::unordered_set<std::string> supported = {
        "model", "input", "instructions", "max_output_tokens", "temperature", "top_p",
        "tools", "tool_choice", "text", "stream", "priority", "metadata",
        "parallel_tool_calls", "reasoning",
    };
    if (!body.is_object()) {
        return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                               "request body must be an object");
    }
    for (auto it = body.begin(); it != body.end(); ++it) {
        if (!supported.contains(it.key())) {
            return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                   "unsupported Responses parameter: " + it.key());
        }
    }
    if (!body.contains("model") || !body["model"].is_string() ||
        body["model"].get<std::string>().empty()) {
        return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                               "request body must include non-empty string 'model'");
    }
    if (!body.contains("input")) {
        return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                               "request body must include 'input'");
    }

    nlohmann::json chat = {{"model", body["model"]}};
    nlohmann::json messages = nlohmann::json::array();
    if (body.contains("instructions")) {
        if (!body["instructions"].is_string()) {
            return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                   "instructions must be a string");
        }
        messages.push_back({{"role", "system"}, {"content", body["instructions"]}});
    }
    if (body.contains("max_output_tokens") &&
        !integer_in_range(body["max_output_tokens"], 1,
                          std::numeric_limits<int>::max())) {
        return foundation::Err<nlohmann::json>(
            foundation::ErrorCode::InvalidArgument,
            "max_output_tokens must be a positive integer");
    }
    if (body.contains("temperature")) {
        if (!body["temperature"].is_number()) {
            return foundation::Err<nlohmann::json>(
                foundation::ErrorCode::InvalidArgument,
                "temperature must be a number");
        }
        const double temperature = body["temperature"].get<double>();
        if (!std::isfinite(temperature) || temperature < 0.0 ||
            temperature > 2.0) {
            return foundation::Err<nlohmann::json>(
                foundation::ErrorCode::InvalidArgument,
                "temperature must be between 0 and 2");
        }
    }
    if (body.contains("top_p")) {
        if (!body["top_p"].is_number()) {
            return foundation::Err<nlohmann::json>(
                foundation::ErrorCode::InvalidArgument,
                "top_p must be a number");
        }
        const double top_p = body["top_p"].get<double>();
        if (!std::isfinite(top_p) || top_p < 0.0 || top_p > 1.0) {
            return foundation::Err<nlohmann::json>(
                foundation::ErrorCode::InvalidArgument,
                "top_p must be between 0 and 1");
        }
    }
    if (body.contains("stream") && !body["stream"].is_boolean()) {
        return foundation::Err<nlohmann::json>(
            foundation::ErrorCode::InvalidArgument,
            "stream must be a boolean");
    }
    if (body.contains("priority") &&
        !integer_in_range(body["priority"], -100, 100)) {
        return foundation::Err<nlohmann::json>(
            foundation::ErrorCode::InvalidArgument,
            "priority must be an integer between -100 and 100");
    }
    if (body.contains("parallel_tool_calls") &&
        !body["parallel_tool_calls"].is_boolean()) {
        return foundation::Err<nlohmann::json>(
            foundation::ErrorCode::InvalidArgument,
            "parallel_tool_calls must be a boolean");
    }
    if (body.contains("reasoning")) {
        if (!body["reasoning"].is_object()) {
            return foundation::Err<nlohmann::json>(
                foundation::ErrorCode::InvalidArgument,
                "reasoning must be an object");
        }
        static const std::unordered_set<std::string> reasoning_fields = {
            "effort",
        };
        auto fields = require_fields(body["reasoning"], reasoning_fields,
                                     "reasoning");
        if (!fields) {
            return foundation::Err<nlohmann::json>(
                fields.error().code, fields.error().message);
        }
        if (!body["reasoning"].contains("effort") ||
            !body["reasoning"]["effort"].is_string()) {
            return foundation::Err<nlohmann::json>(
                foundation::ErrorCode::InvalidArgument,
                "reasoning.effort must be a string");
        }
    }
    if (body.contains("metadata") && !body["metadata"].is_object()) {
        return foundation::Err<nlohmann::json>(
            foundation::ErrorCode::InvalidArgument,
            "metadata must be an object");
    }
    if (body["input"].is_string()) {
        messages.push_back({{"role", "user"}, {"content", body["input"]}});
    } else if (body["input"].is_array()) {
        if (body["input"].empty()) {
            return foundation::Err<nlohmann::json>(
                foundation::ErrorCode::InvalidArgument,
                "input array must not be empty");
        }
        for (const auto& item : body["input"]) {
            if (!item.is_object()) {
                return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                       "each input item must be an object");
            }
            if (item.contains("type") && !item["type"].is_string()) {
                return foundation::Err<nlohmann::json>(
                    foundation::ErrorCode::InvalidArgument,
                    "input item type must be a string");
            }
            const std::string type = item.value("type", "message");
            if (type == "message") {
                static const std::unordered_set<std::string> message_fields = {
                    "type", "role", "content", "id", "status",
                };
                auto fields = require_fields(item, message_fields, "message");
                if (!fields) {
                    return foundation::Err<nlohmann::json>(
                        fields.error().code, fields.error().message);
                }
                if (!item.contains("role") || !item["role"].is_string()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "message requires string role");
                }
                std::string role = item["role"].get<std::string>();
                if (role != "user" && role != "assistant" &&
                    role != "system" && role != "developer") {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "unsupported message role: " + role);
                }
                if (!item.contains("content")) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "message requires content");
                }
                if (item.contains("id") && !item["id"].is_string()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "message id must be a string");
                }
                if (item.contains("status") && !item["status"].is_string()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "message status must be a string");
                }
                auto content = response_content_to_chat(item["content"]);
                if (!content) return foundation::Err<nlohmann::json>(content.error().code, content.error().message);
                if (role == "developer") role = "system";
                messages.push_back({{"role", role}, {"content", std::move(*content)}});
            } else if (type == "function_call_output") {
                static const std::unordered_set<std::string> output_fields = {
                    "type", "call_id", "output", "id", "status",
                };
                auto fields = require_fields(item, output_fields,
                                             "function_call_output");
                if (!fields) {
                    return foundation::Err<nlohmann::json>(
                        fields.error().code, fields.error().message);
                }
                if (!item.contains("call_id") || !item["call_id"].is_string() ||
                    item["call_id"].get<std::string>().empty()) {
                    return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                           "function_call_output requires non-empty string call_id");
                }
                if (!item.contains("output") || !item["output"].is_string()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "function_call_output requires string output");
                }
                if (item.contains("id") && !item["id"].is_string()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "function_call_output id must be a string");
                }
                if (item.contains("status") && !item["status"].is_string()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "function_call_output status must be a string");
                }
                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", item["call_id"]},
                    {"content", item["output"]},
                });
            } else if (type == "function_call") {
                static const std::unordered_set<std::string> call_fields = {
                    "type", "call_id", "id", "name", "arguments", "status",
                };
                auto fields = require_fields(item, call_fields, "function_call");
                if (!fields) {
                    return foundation::Err<nlohmann::json>(
                        fields.error().code, fields.error().message);
                }
                if (!item.contains("name") || !item["name"].is_string() ||
                    item["name"].get<std::string>().empty() ||
                    !item.contains("arguments") ||
                    !item["arguments"].is_string()) {
                    return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                           "function_call requires string name and arguments");
                }
                if (item.contains("id") && !item["id"].is_string()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "function_call id must be a string");
                }
                if (item.contains("status") && !item["status"].is_string()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "function_call status must be a string");
                }
                std::string call_id;
                if (item.contains("call_id")) {
                    if (!item["call_id"].is_string()) {
                        return foundation::Err<nlohmann::json>(
                            foundation::ErrorCode::InvalidArgument,
                            "function_call call_id must be a string");
                    }
                    call_id = item["call_id"].get<std::string>();
                } else if (item.contains("id") && item["id"].is_string()) {
                    call_id = item["id"].get<std::string>();
                }
                if (call_id.empty()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "function_call requires non-empty call_id");
                }
                messages.push_back({
                    {"role", "assistant"},
                    {"content", ""},
                    {"tool_calls", nlohmann::json::array({{{"id", call_id},
                        {"type", "function"},
                        {"function", {{"name", item["name"]},
                                      {"arguments", item["arguments"]}}}}})},
                });
            } else {
                return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                       "unsupported input item type: " + type);
            }
        }
    } else {
        return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                               "input must be a string or array");
    }
    if (messages.empty()) {
        return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                               "input must contain at least one message");
    }
    chat["messages"] = std::move(messages);

    if (body.contains("max_output_tokens")) chat["max_tokens"] = body["max_output_tokens"];
    if (body.contains("temperature")) chat["temperature"] = body["temperature"];
    if (body.contains("top_p")) chat["top_p"] = body["top_p"];
    if (body.contains("priority")) chat["priority"] = body["priority"];
    if (body.contains("parallel_tool_calls")) chat["parallel_tool_calls"] = body["parallel_tool_calls"];
    if (body.contains("reasoning")) chat["reasoning_effort"] = body["reasoning"]["effort"];
    chat["stream"] = body.value("stream", false);
    if (chat["stream"].get<bool>()) chat["stream_options"] = {{"include_usage", true}};

    if (body.contains("tools")) {
        if (!body["tools"].is_array()) {
            return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                   "tools must be an array");
        }
        chat["tools"] = nlohmann::json::array();
        for (const auto& tool : body["tools"]) {
            if (!tool.is_object() || !tool.contains("type") ||
                !tool["type"].is_string() ||
                tool["type"].get<std::string>() != "function") {
                return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                       "only function tools are supported");
            }
            if (!tool.contains("name") || !tool["name"].is_string()) {
                return foundation::Err<nlohmann::json>(
                    foundation::ErrorCode::InvalidArgument,
                    "function tool requires string name");
            }
            const std::string name = tool["name"].get<std::string>();
            if (name.empty()) {
                return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                       "function tool requires name");
            }
            static const std::unordered_set<std::string> tool_fields = {
                "type", "name", "description", "parameters", "strict",
            };
            auto fields = require_fields(tool, tool_fields, "function tool");
            if (!fields) {
                return foundation::Err<nlohmann::json>(
                    fields.error().code, fields.error().message);
            }
            if (tool.contains("description") && !tool["description"].is_string()) {
                return foundation::Err<nlohmann::json>(
                    foundation::ErrorCode::InvalidArgument,
                    "function tool description must be a string");
            }
            if (tool.contains("parameters") && !tool["parameters"].is_object()) {
                return foundation::Err<nlohmann::json>(
                    foundation::ErrorCode::InvalidArgument,
                    "function tool parameters must be an object");
            }
            if (tool.contains("strict") && !tool["strict"].is_boolean()) {
                return foundation::Err<nlohmann::json>(
                    foundation::ErrorCode::InvalidArgument,
                    "function tool strict must be a boolean");
            }
            chat["tools"].push_back({
                {"type", "function"},
                {"function", {
                    {"name", name},
                    {"description", tool.value("description", "")},
                    {"parameters", tool.value("parameters", nlohmann::json::object())},
                    {"strict", tool.value("strict", false)},
                }},
            });
        }
    }
    if (body.contains("tool_choice")) {
        if (body["tool_choice"].is_string()) {
            const std::string choice = body["tool_choice"].get<std::string>();
            if (choice != "auto" && choice != "none" && choice != "required") {
                return foundation::Err<nlohmann::json>(
                    foundation::ErrorCode::InvalidArgument,
                    "tool_choice must be auto, none, or required");
            }
            chat["tool_choice"] = choice;
        } else if (body["tool_choice"].is_object() &&
                   body["tool_choice"].contains("type") &&
                   body["tool_choice"]["type"].is_string() &&
                   body["tool_choice"]["type"].get<std::string>() == "function") {
            static const std::unordered_set<std::string> choice_fields = {
                "type", "name",
            };
            auto fields = require_fields(body["tool_choice"], choice_fields,
                                         "tool_choice");
            if (!fields) {
                return foundation::Err<nlohmann::json>(
                    fields.error().code, fields.error().message);
            }
            if (!body["tool_choice"].contains("name") ||
                !body["tool_choice"]["name"].is_string() ||
                body["tool_choice"]["name"].get<std::string>().empty()) {
                return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                       "function tool_choice requires name");
            }
            const std::string name =
                body["tool_choice"]["name"].get<std::string>();
            chat["tool_choice"] = {{"type", "function"}, {"function", {{"name", name}}}};
        } else {
            return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                   "unsupported tool_choice");
        }
    }
    if (body.contains("text")) {
        if (!body["text"].is_object()) {
            return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                   "text must be an object");
        }
        for (auto it = body["text"].begin(); it != body["text"].end(); ++it) {
            if (it.key() != "format") {
                return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                       "unsupported text parameter: " + it.key());
            }
        }
        if (body["text"].contains("format")) {
            const auto& format = body["text"]["format"];
            if (!format.is_object()) {
                return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                       "text.format must be an object");
            }
            if (format.contains("type") && !format["type"].is_string()) {
                return foundation::Err<nlohmann::json>(
                    foundation::ErrorCode::InvalidArgument,
                    "text.format.type must be a string");
            }
            const std::string type = format.value("type", "text");
            if (type == "json_schema") {
                static const std::unordered_set<std::string> schema_fields = {
                    "type", "name", "description", "schema", "strict",
                };
                auto fields = require_fields(format, schema_fields,
                                             "text.format");
                if (!fields) {
                    return foundation::Err<nlohmann::json>(
                        fields.error().code, fields.error().message);
                }
                if (!format.contains("name") || !format["name"].is_string() ||
                    format["name"].get<std::string>().empty() ||
                    !format.contains("schema") || !format["schema"].is_object()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "json_schema format requires string name and object schema");
                }
                if (format.contains("description") &&
                    !format["description"].is_string()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "json_schema description must be a string");
                }
                if (format.contains("strict") && !format["strict"].is_boolean()) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "json_schema strict must be a boolean");
                }
                auto schema = format;
                schema.erase("type");
                chat["response_format"] = {
                    {"type", "json_schema"},
                    {"json_schema", std::move(schema)},
                };
            } else if (type == "json_object") {
                if (format.size() != 1) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "json_object format accepts only type");
                }
                chat["response_format"] = {{"type", "json_object"}};
            } else if (type == "text") {
                if (format.size() != 1) {
                    return foundation::Err<nlohmann::json>(
                        foundation::ErrorCode::InvalidArgument,
                        "text format accepts only type");
                }
            } else {
                return foundation::Err<nlohmann::json>(foundation::ErrorCode::InvalidArgument,
                                                       "unsupported text format: " + type);
            }
        }
    }
    return foundation::Ok(std::move(chat));
}

nlohmann::json response_usage(const nlohmann::json& chat_usage) {
    const int input = chat_usage.value("prompt_tokens", 0);
    const int output = chat_usage.value("completion_tokens", 0);
    const int cached = chat_usage.value("prompt_tokens_details", nlohmann::json::object())
        .value("cached_tokens", 0);
    return {
        {"input_tokens", input},
        {"input_tokens_details", {{"cached_tokens", cached}}},
        {"output_tokens", output},
        {"output_tokens_details", {{"reasoning_tokens", 0}}},
        {"total_tokens", input + output},
    };
}


nlohmann::json result_to_response(const model::InferenceResult& result,
                                  const nlohmann::json& request,
                                  const std::string& response_id,
                                  const std::string& model_name) {
    nlohmann::json output = nlohmann::json::array();
    if (!result.reasoning_text.empty()) {
        output.push_back({
            {"id", make_id("rs_")}, {"type", "reasoning"},
            {"status", "completed"}, {"summary", nlohmann::json::array()},
            {"content", nlohmann::json::array({{
                {"type", "reasoning_text"}, {"text", result.reasoning_text},
            }})},
        });
    }
    if (!result.text.empty()) {
        output.push_back({
            {"id", make_id("msg_")}, {"type", "message"},
            {"status", "completed"}, {"role", "assistant"},
            {"content", nlohmann::json::array({{
                {"type", "output_text"}, {"text", result.text},
                {"annotations", nlohmann::json::array()},
            }})},
        });
    }
    for (const auto& tool : result.tool_calls) {
        output.push_back({
            {"id", make_id("fc_")}, {"type", "function_call"},
            {"status", "completed"},
            {"call_id", tool.id.empty() ? make_id("call_") : tool.id},
            {"name", tool.function_name},
            {"arguments", tool.function_arguments},
        });
    }
    return {
        {"id", response_id}, {"object", "response"},
        {"created_at", std::time(nullptr)}, {"status", "completed"},
        {"error", nullptr}, {"incomplete_details", nullptr},
        {"instructions", request.value("instructions", nlohmann::json(nullptr))},
        {"max_output_tokens", request.value("max_output_tokens", nlohmann::json(nullptr))},
        {"model", model_name}, {"output", output},
        {"parallel_tool_calls", request.value("parallel_tool_calls", true)},
        {"metadata", request.value("metadata", nlohmann::json::object())},
        {"temperature", request.value("temperature", 1.0)},
        {"top_p", request.value("top_p", 1.0)},
        {"tools", request.value("tools", nlohmann::json::array())},
        {"tool_choice", request.value("tool_choice", nlohmann::json("auto"))},
        {"usage", {
            {"input_tokens", result.prompt_tokens},
            {"input_tokens_details", {{"cached_tokens", result.cached_prompt_tokens}}},
            {"output_tokens", result.completion_tokens},
            {"output_tokens_details", {{"reasoning_tokens", 0}}},
            {"total_tokens", result.prompt_tokens + result.completion_tokens},
        }},
    };
}

struct ToolStreamState {
    std::size_t source_index{0};
    int output_index{0};
    std::string item_id;
    std::string call_id;
    std::string name;
    std::string arguments;
};

struct ResponsesStreamState {
    std::shared_ptr<GenerationSession> session;
    nlohmann::json request;
    std::string response_id;
    std::string model;
    std::string message_id;
    std::string reasoning_id;
    std::optional<int> message_index;
    std::optional<int> reasoning_index;
    int next_output_index{0};
    int sequence{0};
    bool started{false};
    bool completed{false};
    std::string text;
    std::string reasoning;
    std::map<std::size_t, ToolStreamState> tools;
    nlohmann::json usage = nlohmann::json::object();
};

nlohmann::json stream_response_object(const ResponsesStreamState& state,
                                      const std::string& status,
                                      nlohmann::json output,
                                      nlohmann::json error = nullptr) {
    return {
        {"id", state.response_id}, {"object", "response"},
        {"created_at", std::time(nullptr)}, {"status", status},
        {"error", error}, {"incomplete_details", nullptr},
        {"instructions", state.request.value("instructions", nlohmann::json(nullptr))},
        {"max_output_tokens", state.request.value("max_output_tokens", nlohmann::json(nullptr))},
        {"model", state.model}, {"output", std::move(output)},
        {"parallel_tool_calls", state.request.value("parallel_tool_calls", true)},
        {"metadata", state.request.value("metadata", nlohmann::json::object())},
        {"temperature", state.request.value("temperature", 1.0)},
        {"top_p", state.request.value("top_p", 1.0)},
        {"tools", state.request.value("tools", nlohmann::json::array())},
        {"tool_choice", state.request.value("tool_choice", nlohmann::json("auto"))},
        {"usage", response_usage(state.usage)},
    };
}

bool emit_response_event(ResponsesStreamState& state, httplib::DataSink& sink,
                         nlohmann::json event) {
    event["sequence_number"] = state.sequence++;
    const std::string type = event.value("type", "response.event");
    const std::string frame = "event: " + type + "\ndata: " +
        event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n\n";
    return sink.write(frame.data(), frame.size());
}

bool start_response_stream(ResponsesStreamState& state, httplib::DataSink& sink) {
    if (state.started) return true;
    state.started = true;
    auto response = stream_response_object(state, "in_progress", nlohmann::json::array());
    return emit_response_event(state, sink, {{"type", "response.created"}, {"response", response}}) &&
           emit_response_event(state, sink, {{"type", "response.in_progress"}, {"response", response}});
}

bool ensure_message_stream(ResponsesStreamState& state, httplib::DataSink& sink) {
    if (state.message_index) return true;
    state.message_index = state.next_output_index++;
    state.message_id = make_id("msg_");
    const nlohmann::json item = {
        {"id", state.message_id}, {"type", "message"}, {"status", "in_progress"},
        {"role", "assistant"}, {"content", nlohmann::json::array()},
    };
    return emit_response_event(state, sink, {
        {"type", "response.output_item.added"}, {"output_index", *state.message_index},
        {"item", item},
    }) && emit_response_event(state, sink, {
        {"type", "response.content_part.added"}, {"item_id", state.message_id},
        {"output_index", *state.message_index}, {"content_index", 0},
        {"part", {{"type", "output_text"}, {"text", ""},
                  {"annotations", nlohmann::json::array()}}},
    });
}

bool ensure_reasoning_stream(ResponsesStreamState& state, httplib::DataSink& sink) {
    if (state.reasoning_index) return true;
    state.reasoning_index = state.next_output_index++;
    state.reasoning_id = make_id("rs_");
    const nlohmann::json item = {
        {"id", state.reasoning_id}, {"type", "reasoning"}, {"status", "in_progress"},
        {"summary", nlohmann::json::array()}, {"content", nlohmann::json::array()},
    };
    return emit_response_event(state, sink, {
        {"type", "response.output_item.added"}, {"output_index", *state.reasoning_index},
        {"item", item},
    }) && emit_response_event(state, sink, {
        {"type", "response.content_part.added"}, {"item_id", state.reasoning_id},
        {"output_index", *state.reasoning_index}, {"content_index", 0},
        {"part", {{"type", "reasoning_text"}, {"text", ""}}},
    });
}

bool apply_sanitized_generation_delta(ResponsesStreamState& state,
                                      httplib::DataSink& sink,
                                      const model::InferenceDelta& delta) {
    if (!delta.reasoning_text.empty()) {
        if (!ensure_reasoning_stream(state, sink)) return false;
        state.reasoning += delta.reasoning_text;
        if (!emit_response_event(state, sink, {
            {"type", "response.reasoning_text.delta"},
            {"item_id", state.reasoning_id},
            {"output_index", *state.reasoning_index}, {"content_index", 0},
            {"delta", delta.reasoning_text},
        })) return false;
    }
    if (!delta.content.empty()) {
        if (!ensure_message_stream(state, sink)) return false;
        state.text += delta.content;
        if (!emit_response_event(state, sink, {
            {"type", "response.output_text.delta"},
            {"item_id", state.message_id},
            {"output_index", *state.message_index}, {"content_index", 0},
            {"delta", delta.content},
        })) return false;
    }
    for (const auto& source : delta.tool_calls) {
        auto [iterator, inserted] = state.tools.try_emplace(source.index);
        auto& tool = iterator->second;
        if (inserted) {
            tool.source_index = source.index;
            tool.output_index = state.next_output_index++;
            tool.item_id = make_id("fc_");
            tool.call_id = source.id.empty() ? make_id("call_") : source.id;
            tool.name = source.function_name;
            const nlohmann::json item = {
                {"id", tool.item_id}, {"type", "function_call"},
                {"status", "in_progress"}, {"call_id", tool.call_id},
                {"name", tool.name}, {"arguments", ""},
            };
            if (!emit_response_event(state, sink, {
                {"type", "response.output_item.added"},
                {"output_index", tool.output_index}, {"item", item},
            })) return false;
        } else {
            if (!source.id.empty()) tool.call_id = source.id;
            if (!source.function_name.empty()) tool.name += source.function_name;
        }
        if (!source.function_arguments.empty()) {
            tool.arguments += source.function_arguments;
            if (!emit_response_event(state, sink, {
                {"type", "response.function_call_arguments.delta"},
                {"item_id", tool.item_id},
                {"output_index", tool.output_index},
                {"delta", source.function_arguments},
            })) return false;
        }
    }
    return true;
}

bool apply_generation_delta(ResponsesStreamState& state,
                            httplib::DataSink& sink,
                            const model::InferenceDelta& source) {
    return apply_sanitized_generation_delta(
        state, sink, state.session->utf8.on_delta(source));
}

nlohmann::json completed_stream_output(const ResponsesStreamState& state) {
    std::vector<std::pair<int, nlohmann::json>> indexed;
    if (state.reasoning_index) {
        indexed.push_back({*state.reasoning_index, {
            {"id", state.reasoning_id}, {"type", "reasoning"}, {"status", "completed"},
            {"summary", nlohmann::json::array()},
            {"content", nlohmann::json::array({{{"type", "reasoning_text"},
                                                  {"text", state.reasoning}}})},
        }});
    }
    if (state.message_index) {
        indexed.push_back({*state.message_index, {
            {"id", state.message_id}, {"type", "message"}, {"status", "completed"},
            {"role", "assistant"},
            {"content", nlohmann::json::array({{{"type", "output_text"},
                                                  {"text", state.text},
                                                  {"annotations", nlohmann::json::array()}}})},
        }});
    }
    for (const auto& [_, tool] : state.tools) {
        indexed.push_back({tool.output_index, {
            {"id", tool.item_id}, {"type", "function_call"}, {"status", "completed"},
            {"call_id", tool.call_id}, {"name", tool.name}, {"arguments", tool.arguments},
        }});
    }
    std::sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    nlohmann::json output = nlohmann::json::array();
    for (auto& [_, item] : indexed) output.push_back(std::move(item));
    return output;
}

bool finish_response_stream(ResponsesStreamState& state, httplib::DataSink& sink) {
    if (state.completed) return true;
    if (state.reasoning_index) {
        if (!emit_response_event(state, sink, {
            {"type", "response.reasoning_text.done"}, {"item_id", state.reasoning_id},
            {"output_index", *state.reasoning_index}, {"content_index", 0},
            {"text", state.reasoning},
        })) return false;
        if (!emit_response_event(state, sink, {
            {"type", "response.content_part.done"}, {"item_id", state.reasoning_id},
            {"output_index", *state.reasoning_index}, {"content_index", 0},
            {"part", {{"type", "reasoning_text"}, {"text", state.reasoning}}},
        })) return false;
    }
    if (state.message_index) {
        if (!emit_response_event(state, sink, {
            {"type", "response.output_text.done"}, {"item_id", state.message_id},
            {"output_index", *state.message_index}, {"content_index", 0},
            {"text", state.text},
        })) return false;
        if (!emit_response_event(state, sink, {
            {"type", "response.content_part.done"}, {"item_id", state.message_id},
            {"output_index", *state.message_index}, {"content_index", 0},
            {"part", {{"type", "output_text"}, {"text", state.text},
                      {"annotations", nlohmann::json::array()}}},
        })) return false;
    }
    const auto output = completed_stream_output(state);
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto& item = output[index];
        if (item.value("type", "") == "function_call") {
            if (!emit_response_event(state, sink, {
                {"type", "response.function_call_arguments.done"},
                {"item_id", item["id"]}, {"output_index", index},
                {"name", item["name"]}, {"arguments", item["arguments"]},
            })) return false;
        }
        if (!emit_response_event(state, sink, {
            {"type", "response.output_item.done"}, {"output_index", index}, {"item", item},
        })) return false;
    }
    state.completed = true;
    return emit_response_event(state, sink, {
        {"type", "response.completed"},
        {"response", stream_response_object(state, "completed", output)},
    });
}

bool fail_response_stream(ResponsesStreamState& state, httplib::DataSink& sink,
                          const nlohmann::json& error) {
    if (state.completed) return true;
    state.completed = true;
    auto response = stream_response_object(state, "failed", nlohmann::json::array(), error);
    return emit_response_event(state, sink, {{"type", "response.failed"}, {"response", response}});
}


} // namespace

void handle_embeddings(const httplib::Request& req, httplib::Response& resp,
                       const GatewayDeps& deps) {
    if (!require_json_media_type(req, resp)) return;
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& error) {
        write_error(resp, 400, "invalid_json", error.what());
        return;
    }
    if (!body.is_object()) {
        write_error(resp, 400, "invalid_request_error",
                    "request body must be an object");
        return;
    }
    if (deps.compatibility_profile == CompatibilityProfile::StrictOpenAI) {
        static const std::unordered_set<std::string> supported{
            "model", "input", "encoding_format", "dimensions", "user",
        };
        for (const auto& field : body.items()) {
            if (!supported.contains(field.key())) {
                write_error(resp, 400, "unsupported_parameter",
                            "unsupported Embeddings parameter: " + field.key());
                return;
            }
        }
    }
    if (!body.contains("model") || !body["model"].is_string() ||
        body["model"].get<std::string>().empty()) {
        write_error(resp, 400, "missing_model", "request body must include 'model'");
        return;
    }
    const std::string requested_model = body["model"].get<std::string>();
    const auto resolved_model = resolve_model_name(deps, requested_model);
    if (!resolved_model) {
        write_error(resp, 404, "model_not_found", resolved_model.error().message);
        return;
    }
    const std::string& model_name = resolved_model->resolved;
    auto info = deps.coordinator.registry().get_info_result(model_name);
    if (!info) {
        write_error(resp, 404, "model_not_found", info.error().message);
        return;
    }
    if (!info->supports("embeddings")) {
        write_error(resp, 400, "unsupported_capability",
                    "model does not support embeddings: " + model_name);
        return;
    }

    model::EmbeddingRequest embedding_request;
    if (body.contains("input") && body["input"].is_string()) {
        embedding_request.inputs.push_back(body["input"].get<std::string>());
    } else if (body.contains("input") && body["input"].is_array()) {
        if (body["input"].empty() || body["input"].size() > 256) {
            write_error(resp, 400, "invalid_input", "input array must contain 1 to 256 strings");
            return;
        }
        for (const auto& input : body["input"]) {
            if (!input.is_string()) {
                write_error(resp, 400, "invalid_input", "every input must be a string");
                return;
            }
            embedding_request.inputs.push_back(input.get<std::string>());
        }
    } else {
        write_error(resp, 400, "invalid_input", "input must be a string or array of strings");
        return;
    }
    std::size_t total_bytes = 0;
    for (const auto& input : embedding_request.inputs) {
        total_bytes += input.size();
        if (input.empty() || input.size() > 1024 * 1024 || total_bytes > 4 * 1024 * 1024) {
            write_error(resp, 400, "invalid_input", "embedding input is empty or too large");
            return;
        }
    }
    if (body.contains("dimensions")) {
        if (!body["dimensions"].is_number_integer() ||
            body["dimensions"].get<int>() <= 0 ||
            body["dimensions"].get<int>() > 65536) {
            write_error(resp, 400, "invalid_dimensions", "dimensions must be a positive integer");
            return;
        }
        embedding_request.dimensions = body["dimensions"].get<int>();
    }
    if (body.contains("user") &&
        (!body["user"].is_string() ||
         body["user"].get<std::string>().size() > 64)) {
        write_error(resp, 400, "invalid_user",
                    "user must be a string of at most 64 characters");
        return;
    }
    const std::string encoding = body.value("encoding_format", "float");
    if (encoding != "float" && encoding != "base64") {
        write_error(resp, 400, "unsupported_encoding", "encoding_format must be float or base64");
        return;
    }
    const int priority = body.contains("priority") && body["priority"].is_number_integer()
        ? body["priority"].get<int>() : 0;
    auto slot = acquire_for_request(req, deps, model_name, priority);
    if (!slot) {
        const int status = slot.error().code == foundation::ErrorCode::NotFound ? 404 : 503;
        write_error(resp, status, "embedding_unavailable", slot.error().message);
        return;
    }
    struct SlotGuard {
        model::BackendCoordinator& coordinator;
        std::string model;
        int slot;
        ~SlotGuard() { (void)coordinator.release_slot(model, slot); }
    } guard{deps.coordinator, model_name, *slot};

    auto result = deps.coordinator.embed(model_name, *slot, embedding_request,
        [&req] { return req.is_connection_closed(); });
    if (!result) {
        const int status = result.error().code == foundation::ErrorCode::InvalidArgument ? 400
            : result.error().code == foundation::ErrorCode::Cancelled ? 499 : 500;
        record_request(deps, requested_model, model::InferenceResult{}, status, *slot,
                       0.0, 0, model_name);
        write_error(resp, status, "embedding_failed", result.error().message);
        return;
    }
    nlohmann::json data = nlohmann::json::array();
    for (std::size_t index = 0; index < result->embeddings.size(); ++index) {
        data.push_back({
            {"object", "embedding"},
            {"index", index},
            {"embedding", encoding == "base64"
                ? nlohmann::json(base64_floats(result->embeddings[index]))
                : nlohmann::json(result->embeddings[index])},
        });
    }
    model::InferenceResult metrics_result;
    metrics_result.prompt_tokens = result->prompt_tokens;
    metrics_result.duration_ms = result->duration_ms;
    record_request(deps, requested_model, metrics_result, 200, *slot,
                   0.0, 0, model_name);
    write_json(resp, 200, {
        {"object", "list"},
        {"data", data},
        {"model", requested_model},
        {"usage", {
            {"prompt_tokens", result->prompt_tokens},
            {"total_tokens", result->prompt_tokens},
        }},
    });
}

void handle_responses(const httplib::Request& req, httplib::Response& resp,
                      const GatewayDeps& deps) {
    if (!require_json_media_type(req, resp)) return;
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
        write_error(resp, 400, "invalid_json", "invalid JSON");
        return;
    }
    if (deps.compatibility_profile == CompatibilityProfile::StrictOpenAI &&
        request.contains("priority")) {
        write_error(resp, 400, "unsupported_parameter",
                    "unsupported Responses parameter: priority");
        return;
    }
    auto chat_body = responses_to_chat(request);
    if (!chat_body) {
        const std::string& message = chat_body.error().message;
        const std::string code =
            message.starts_with("unsupported Responses parameter:")
                ? "unsupported_parameter" : "invalid_request_error";
        write_error(resp, 400, code, message);
        return;
    }
    const std::string requested_model = (*chat_body)["model"].get<std::string>();
    const auto resolved_model = resolve_model_name(deps, requested_model);
    if (!resolved_model) {
        write_error(resp, 404, "model_not_found", resolved_model.error().message);
        return;
    }
    const std::string& model_name = resolved_model->resolved;
    auto info = deps.coordinator.registry().get_info_result(model_name);
    if (!info) {
        write_error(resp, 404, "model_not_found", info.error().message);
        return;
    }
    if (!info->has_vision && response_input_uses_vision(request["input"])) {
        write_error(resp, 400, "unsupported_capability",
                    "model does not support image input: " + model_name);
        return;
    }
    if (!info->supports("responses")) {
        write_error(resp, 400, "unsupported_capability",
                    "model does not support Responses API: " + model_name);
        return;
    }
    const bool stream = request.value("stream", false);
    auto inference_request = parse_openai_chat_request(*chat_body, true);
    if (!inference_request) {
        write_error(resp, 400, "invalid_request_error",
                    inference_request.error().message);
        return;
    }
    if (!stream) {
        auto acquired = acquire_generation_slot(
            req, resp, deps, *chat_body, requested_model, model_name);
        if (!acquired) return;
        GenerationSession session(
            deps.coordinator, deps.metrics, deps.stats_db, deps.events,
            acquired->slot_id, requested_model, model_name,
            acquired->reservation_key,
            acquired->voice_session_token.value_or(0),
            deps.voice_session_grace_ms);
        auto result = session.run(*inference_request);
        if (!result) {
            const auto error = map_openai_error(result.error().code);
            write_error(resp, error.status, error.code, result.error().message);
            session.finish_once(false, error.status, "inference_error");
            return;
        }
        write_json(resp, 200, result_to_response(
            *result, request, make_id("resp_"), requested_model));
        session.finish_once(false, 200, "completed");
        return;
    }
    auto acquired = acquire_generation_slot(
        req, resp, deps, *chat_body, requested_model, model_name);
    if (!acquired) return;
    auto session = std::make_shared<GenerationSession>(
        deps.coordinator, deps.metrics, deps.stats_db, deps.events,
        acquired->slot_id, requested_model, model_name,
        acquired->reservation_key,
        acquired->voice_session_token.value_or(0),
        deps.voice_session_grace_ms);
    auto state = std::make_shared<ResponsesStreamState>();
    state->session = session;
    state->request = request;
    state->response_id = make_id("resp_");
    state->model = model_name;
    session->start(std::move(*inference_request));

    resp.status = 200;
    resp.set_header("Cache-Control", "no-cache");
    resp.set_header("Connection", "keep-alive");
    resp.set_chunked_content_provider(
        "text/event-stream",
        [state](std::size_t, httplib::DataSink& sink) {
            try {
                if (!start_response_stream(*state, sink)) {
                    state->session->finish_once(true, 499, "start_write_failed");
                    return false;
                }
                std::unique_lock lock(state->session->mtx);
                while (!state->session->cv.wait_for(
                    lock, std::chrono::seconds{2}, [&] {
                        return !state->session->delta_queue.empty() ||
                               state->session->inference_done ||
                               state->session->aborted.load();
                    })) {
                    lock.unlock();
                    if (!sink.write(": \n\n", 4)) {
                        state->session->finish_once(
                            true, 499, "heartbeat_write_failed");
                        return false;
                    }
                    lock.lock();
                }
                if (state->session->aborted.load() &&
                    !state->session->inference_done &&
                    state->session->delta_queue.empty()) {
                    lock.unlock();
                    state->session->finish_once(true, 499, "aborted");
                    return false;
                }
                if (!state->session->delta_queue.empty()) {
                    std::deque<model::InferenceDelta> deltas;
                    deltas.swap(state->session->delta_queue);
                    state->session->pending_bytes = 0;
                    lock.unlock();
                    state->session->cv.notify_all();
                    if (!sink.is_writable()) {
                        state->session->finish_once(
                            true, 499, "sink_not_writable");
                        return false;
                    }
                    for (const auto& delta : deltas) {
                        if (!apply_generation_delta(*state, sink, delta)) {
                            state->session->finish_once(
                                true, 499, "chunk_write_failed");
                            return false;
                        }
                    }
                    return true;
                }
                const bool inference_error = state->session->inference_error;
                const auto error_code = state->session->error_code;
                const auto error_message = state->session->error_msg;
                const auto result = state->session->final_result;
                lock.unlock();

                const auto trailing = state->session->utf8.finish();
                if (!apply_sanitized_generation_delta(*state, sink, trailing)) {
                    state->session->finish_once(
                        true, 499, "trailing_chunk_write_failed");
                    return false;
                }
                if (inference_error) {
                    const auto error = map_openai_error(error_code);
                    if (!fail_response_stream(*state, sink, {
                        {"code", error.code}, {"message", error_message},
                    })) {
                        state->session->finish_once(
                            true, 499, "error_write_failed");
                        return false;
                    }
                    state->session->finish_once(
                        false, error.status, "inference_error");
                } else {
                    if (result) {
                        state->usage = {
                            {"prompt_tokens", result->prompt_tokens},
                            {"prompt_tokens_details", {
                                {"cached_tokens", result->cached_prompt_tokens},
                            }},
                            {"completion_tokens", result->completion_tokens},
                        };
                    }
                    if (!finish_response_stream(*state, sink)) {
                        state->session->finish_once(
                            true, 499, "done_write_failed");
                        return false;
                    }
                    state->session->finish_once(false, 200, "completed");
                }
                sink.done();
                return false;
            } catch (...) {
                state->session->finish_once(
                    true, 500, "provider_exception");
                return false;
            }
        },
        [state](bool success) {
            state->session->finish_once(
                !success, success ? 200 : 499,
                success ? "resource_releaser_success" : "resource_releaser");
        });
}

} // namespace inferdeck::gateway
