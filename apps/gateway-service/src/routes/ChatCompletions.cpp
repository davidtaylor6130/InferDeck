#include "routes/ChatCompletions.hpp"
#include "RuntimeActivity.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "config/ConfigLoader.hpp"
#include "core/Base64.hpp"
#include "core/Config.hpp"
#include "core/Logger.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <sstream>
#include <chrono>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <memory>
#include <vector>
#include <unordered_map>
#include <optional>
#include <string_view>
#include <thread>
#include <cstdint>
#include <cstdlib>
using json = nlohmann::json;

namespace {

static std::string MakeModelId(const std::string& full_path) {
    if (full_path.empty()) return "local-model";
    std::filesystem::path p(full_path);
    std::string name = p.stem().string();
    for (auto& c : name) {
        if (c == ' ' || c == '_' || c == '-') c = '-';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

static const std::vector<std::string> kQuantSuffixes = {
    "-q4_k_m", "-q4_k_s", "-q4_0", "-q5_k_m", "-q5_k_s", "-q5_0",
    "-q6_k", "-q6_k_m", "-q8_0", "-q2_k", "-q3_k_m", "-q3_k_s",
    "-iq1_s", "-iq2_s", "-iq2_xs", "-iq2_xxs", "-iq3_s", "-iq3_xs",
    "-iq3_xxs", "-iq4_nl", "-iq4_xs", "-iq4_xxs",
    "-iq1_m", "-iq2_m", "-iq3_m",
    "-mxfp4", "-mx4", "-mx6", "-mx8",
    "-ud-iq3_xxs", "-ud-iq2_xs",
    "-f16", "-f32", "-bf16",
    "-gguf",
    ".q4_k_m", ".q4_k_s", ".q4_0", ".q5_k_m", ".q5_k_s", ".q5_0",
    ".q6_k", ".q6_k_m", ".q8_0", ".q2_k", ".q3_k_m", ".q3_k_s",
    ".iq1_s", ".iq2_s", ".iq2_xs", ".iq2_xxs", ".iq3_s", ".iq3_xs",
    ".iq3_xxs", ".iq4_nl", ".iq4_xs", ".iq4_xxs",
    ".iq1_m", ".iq2_m", ".iq3_m",
    ".mxfp4", ".mx4", ".mx6", ".mx8",
    ".ud-iq3_xxs", ".ud-iq2_xs",
    ".f16", ".f32", ".bf16",
    ".gguf"
};

static std::string NormalizeModelName(const std::string& name) {
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string clean = name;
    for (const auto& suffix : kQuantSuffixes) {
        size_t pos = lower.rfind(suffix);
        if (pos != std::string::npos && pos + suffix.size() == lower.size()) {
            clean = name.substr(0, pos);
            break;
        }
    }

    for (auto& c : clean) {
        if (c == ' ' || c == '_' || c == '-') c = '-';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return clean;
}

static std::string MakeCleanModelId(const std::string& full_path) {
    if (full_path.empty()) return "local-model";
    std::filesystem::path p(full_path);
    std::string name = p.stem().string();
    return NormalizeModelName(name);
}

static std::string NormalizeId(const std::string& id) {
    std::string result = id;
    constexpr size_t latest_suffix_len = 7;
    if (result.size() > latest_suffix_len && result.compare(result.size() - latest_suffix_len, latest_suffix_len, ":latest") == 0) {
        result = result.substr(0, result.size() - latest_suffix_len);
    }
    for (auto& c : result) {
        if (c == ' ' || c == '_' || c == ':') c = '-';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

struct ModelEntry {
    std::string path;
    std::string clean_id;
    std::string full_id;
};

static std::vector<ModelEntry> g_model_cache;
static std::mutex g_model_cache_mutex;
static bool g_cache_populated = false;
static std::mutex g_switch_mutex;

static void PopulateModelCache(const std::string& model_dir) {
    if (g_cache_populated) return;

    std::vector<std::string> extensions = {".gguf", ".bin", ".ggml"};
    std::unordered_map<std::string, ModelEntry> clean_map;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(model_dir)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            std::string filename = entry.path().filename().string();
            if (filename.find("mmproj") == 0) continue;
            for (const auto& e : extensions) {
                if (ext == e) {
                    ModelEntry me;
                    me.path = entry.path().string();
                    me.clean_id = MakeCleanModelId(me.path);
                    me.full_id = MakeModelId(me.path);

                    if (clean_map.find(me.clean_id) == clean_map.end()) {
                        clean_map[me.clean_id] = me;
                    }
                    g_model_cache.push_back(me);
                    break;
                }
            }
        }
    }

    g_cache_populated = true;
}

static void RefreshModelCache(const std::string& model_dir) {
    g_model_cache.clear();
    g_cache_populated = false;
    PopulateModelCache(model_dir);
}

static std::string FindModelPath(const std::string& model_id) {
    std::lock_guard<std::mutex> lock(g_model_cache_mutex);

    std::string normalized = NormalizeId(model_id);
    if (normalized.empty()) return "";

    std::string model_dir;
    try {
        auto full = inferdeck::core::Config::Load(inferdeck::gateway::GetDefaultConfigPath());
        model_dir = full.model.directory;
    } catch (...) { return ""; }

    if (model_dir.empty() || !std::filesystem::exists(model_dir)) return "";

    if (normalized == "gpt-oss-20b" || normalized == "openai-gpt-oss-20b") {
        auto path = std::filesystem::path(model_dir) / "openai_gpt-oss-20b-GGUF" / "openai_gpt-oss-20b-MXFP4.gguf";
        if (std::filesystem::exists(path)) return path.string();
    }

    PopulateModelCache(model_dir);

    for (const auto& entry : g_model_cache) {
        if (entry.clean_id == normalized || entry.full_id == normalized) {
            return entry.path;
        }
    }

    for (const auto& entry : g_model_cache) {
        if (entry.clean_id.find(normalized) != std::string::npos ||
            normalized.find(entry.clean_id) != std::string::npos) {
            return entry.path;
        }
    }

    for (const auto& entry : g_model_cache) {
        std::string short_clean = entry.clean_id;
        size_t dash = short_clean.find_last_of('-');
        if (dash != std::string::npos) {
            std::string prefix = short_clean.substr(0, dash);
            if (normalized.find(prefix) != std::string::npos || prefix.find(normalized) != std::string::npos) {
                return entry.path;
            }
        }
    }

    RefreshModelCache(model_dir);

    for (const auto& entry : g_model_cache) {
        if (entry.clean_id == normalized || entry.full_id == normalized ||
            entry.clean_id.find(normalized) != std::string::npos ||
            normalized.find(entry.clean_id) != std::string::npos) {
            return entry.path;
        }
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(model_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = NormalizeId(entry.path().stem().string());
        if (entry.path().extension() == ".gguf" &&
            (filename.find(normalized) != std::string::npos || normalized.find(filename) != std::string::npos)) {
            return entry.path().string();
        }
    }

    return "";
}

static std::string GetCurrentCleanModelId() {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    std::string model_name = engine.GetModelName();
    if (model_name.empty()) return "";
    return NormalizeModelName(model_name);
}

}

namespace inferdeck::gateway::routes {

static std::string MakeId() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return "chatcmpl-" + std::to_string(ms);
}

static std::string SseDeltaChunk(const std::string& id, const std::string& model, const json& delta, const json& finish_reason = nullptr);
static std::string SseErrorChunk(const std::string& id, const std::string& model, const std::string& message);

static std::size_t CountSseDataChunks(const std::string& stream) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = stream.find("data: ", pos)) != std::string::npos) {
        ++count;
        pos += 6;
    }
    return count;
}

static std::string ResponseFinishReason(const nlohmann::json& response) {
    if (!response.contains("choices") || !response["choices"].is_array() || response["choices"].empty()) return "stop";
    const auto& choice = response["choices"][0];
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
        return choice["finish_reason"].get<std::string>();
    }
    return "stop";
}

static std::size_t ResponseToolCallCount(const nlohmann::json& response) {
    if (!response.contains("choices") || !response["choices"].is_array() || response["choices"].empty()) return 0;
    const auto& choice = response["choices"][0];
    if (!choice.contains("message") || !choice["message"].is_object()) return 0;
    const auto& message = choice["message"];
    if (!message.contains("tool_calls") || !message["tool_calls"].is_array()) return 0;
    return message["tool_calls"].size();
}

static std::string TrimCopy(std::string text);
static std::string SafeLogText(std::string text, std::size_t limit);
static std::string RemoveQwenToolCallBlocks(std::string text);
std::string ExtractAssistantReasoningContent(const std::string& content);

static std::string JsonArgumentString(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_object() || value.is_array()) return value.dump();
    if (value.is_null()) return "{}";
    return value.dump();
}

struct ToolNameRegistry {
    std::vector<std::string> names;
    bool has_tools = false;
};

static std::string LowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

static std::string BaseToolName(std::string name) {
    name = LowerCopy(TrimCopy(std::move(name)));
    if (name.rfind("tool.", 0) == 0) name = name.substr(5);
    const auto pos = name.find_last_of("./:-");
    if (pos != std::string::npos && pos + 1 < name.size()) name = name.substr(pos + 1);
    return name;
}

static void AddToolName(ToolNameRegistry& registry, const nlohmann::json& value) {
    if (!value.is_string()) return;
    auto name = TrimCopy(value.get<std::string>());
    if (name.empty()) return;
    registry.names.push_back(std::move(name));
    registry.has_tools = true;
}

static ToolNameRegistry BuildToolNameRegistry(const nlohmann::json& tools) {
    ToolNameRegistry registry;
    if (!tools.is_array()) return registry;
    for (const auto& tool : tools) {
        if (tool.is_string()) {
            AddToolName(registry, tool);
        } else if (tool.is_object()) {
            if (tool.contains("function") && tool["function"].is_object()) {
                AddToolName(registry, tool["function"].value("name", ""));
            }
            AddToolName(registry, tool.value("name", ""));
        }
    }
    return registry;
}

static std::string ResolveToolName(const ToolNameRegistry& registry,
                                   const std::string& requested_name,
                                   const nlohmann::json& arguments) {
    const auto requested = TrimCopy(requested_name);
    const auto requested_base = BaseToolName(requested);
    if (!registry.has_tools) {
        if (!requested_base.empty()) return requested_base;
        if (arguments.is_object() && arguments.contains("command")) return "bash";
        return requested;
    }

    auto find_by = [&](const std::vector<std::string>& candidates) -> std::string {
        for (const auto& candidate : candidates) {
            const auto candidate_lower = LowerCopy(candidate);
            const auto candidate_base = BaseToolName(candidate);
            for (const auto& actual : registry.names) {
                const auto actual_lower = LowerCopy(actual);
                const auto actual_base = BaseToolName(actual);
                if (actual_lower == candidate_lower || actual_base == candidate_lower ||
                    actual_lower == candidate_base || actual_base == candidate_base) {
                    return actual;
                }
            }
        }
        return "";
    };

    if (!requested.empty()) {
        if (auto exact = find_by({requested}); !exact.empty()) return exact;
    }

    std::vector<std::string> aliases;
    const bool command_args = arguments.is_object() && arguments.contains("command");
    if (command_args || requested_base == "bash" || requested_base == "shell" ||
        requested_base == "command" || requested_base == "terminal" ||
        requested_base == "exec" || requested_base == "run_command") {
        aliases = {"bash", "shell", "run_command", "terminal", "command", "exec"};
    } else if (requested_base == "glob") {
        aliases = {"glob", "file_glob", "list_files"};
    } else if (requested_base == "read") {
        aliases = {"read", "read_file", "file_read"};
    } else if (requested_base == "grep" || requested_base == "search") {
        aliases = {"grep", "search", "rg"};
    } else if (requested_base == "write") {
        aliases = {"write", "write_file"};
    } else if (requested_base == "edit") {
        aliases = {"edit", "apply_patch"};
    } else if (!requested_base.empty()) {
        aliases = {requested_base};
    }

    if (!aliases.empty()) {
        if (auto resolved = find_by(aliases); !resolved.empty()) return resolved;
    }

    return "";
}

static std::string MakeToolCallId(std::size_t index) {
    return "call_inferdeck_" + std::to_string(index + 1);
}

static nlohmann::json NormalizeOpenAiToolCallsWithRegistry(const nlohmann::json& tool_calls,
                                                           const ToolNameRegistry& registry) {
    nlohmann::json normalized = nlohmann::json::array();
    if (!tool_calls.is_array()) return normalized;

    std::size_t index = 0;
    std::vector<std::string> seen;
    for (const auto& raw : tool_calls) {
        if (!raw.is_object()) continue;
        nlohmann::json item;
        item["id"] = raw.value("id", MakeToolCallId(index));
        item["type"] = raw.value("type", "function");
        if (!raw.contains("function") || !raw["function"].is_object()) continue;
        const auto& function = raw["function"];
        const auto name = function.value("name", "");
        const auto resolved = ResolveToolName(registry, name, function.contains("arguments") ? function["arguments"] : nlohmann::json::object());
        if (resolved.empty()) continue;
        const auto arguments = function.contains("arguments") ? JsonArgumentString(function["arguments"]) : "{}";
        const auto dedupe_key = resolved + "\n" + arguments;
        if (std::find(seen.begin(), seen.end(), dedupe_key) != seen.end()) continue;
        seen.push_back(dedupe_key);
        item["function"] = {
            {"name", resolved},
            {"arguments", arguments}
        };
        normalized.push_back(std::move(item));
        ++index;
    }
    return normalized;
}

nlohmann::json NormalizeOpenAiToolCalls(const nlohmann::json& tool_calls) {
    return NormalizeOpenAiToolCallsWithRegistry(tool_calls, ToolNameRegistry{});
}

static nlohmann::json RawToolCallFromPayload(const nlohmann::json& parsed,
                                             std::size_t index,
                                             const ToolNameRegistry& registry,
                                             const std::string& requested_override = "") {
    nlohmann::json raw_call;
    raw_call["id"] = MakeToolCallId(index);
    raw_call["type"] = "function";

    if (parsed.contains("function") && parsed["function"].is_object()) {
        auto function = parsed["function"];
        const auto requested = !requested_override.empty() ? requested_override : function.value("name", "");
        const auto resolved = ResolveToolName(registry, requested, function.contains("arguments") ? function["arguments"] : nlohmann::json::object());
        if (resolved.empty()) return nlohmann::json::object();
        function["name"] = resolved;
        raw_call["function"] = std::move(function);
        return raw_call;
    }

    const auto name = !requested_override.empty() ? requested_override : parsed.value("name", parsed.value("function", ""));
    if (!name.empty()) {
        const auto arguments = parsed.contains("arguments")
            ? parsed["arguments"]
            : (!requested_override.empty() ? parsed : nlohmann::json::object());
        const auto resolved = ResolveToolName(registry, name, arguments);
        if (resolved.empty()) return nlohmann::json::object();
        raw_call["function"] = {
            {"name", resolved},
            {"arguments", arguments}
        };
        return raw_call;
    }

    if (parsed.contains("command") && parsed["command"].is_string()) {
        const auto resolved = ResolveToolName(registry, "bash", parsed);
        if (resolved.empty()) return nlohmann::json::object();
        raw_call["function"] = {
            {"name", resolved},
            {"arguments", parsed}
        };
        return raw_call;
    }

    return nlohmann::json::object();
}

static nlohmann::json NormalizeToolCallPayload(const nlohmann::json& parsed,
                                               std::size_t start_index,
                                               const ToolNameRegistry& registry,
                                               const std::string& requested_override = "") {
    nlohmann::json raw_calls = nlohmann::json::array();

    if (parsed.is_array()) {
        for (const auto& item : parsed) {
            auto raw_call = RawToolCallFromPayload(item, start_index + raw_calls.size(), registry, requested_override);
            if (!raw_call.empty()) raw_calls.push_back(std::move(raw_call));
        }
    } else if (parsed.is_object()) {
        if (parsed.contains("tool_calls")) {
            return NormalizeToolCallPayload(parsed["tool_calls"], start_index, registry, requested_override);
        }
        auto raw_call = RawToolCallFromPayload(parsed, start_index, registry, requested_override);
        if (!raw_call.empty()) raw_calls.push_back(std::move(raw_call));
    }

    return NormalizeOpenAiToolCallsWithRegistry(raw_calls, registry);
}

static std::optional<std::pair<std::string, std::pair<std::size_t, std::size_t>>> FindLabeledJsonPayload(
    const std::string& text,
    std::size_t search_from) {
    const std::array<std::string_view, 3> labels = {"assistant_tool_calls_json:", "tool_calls:", "tool_call:"};

    std::size_t label_pos = std::string::npos;
    std::string_view matched_label;
    for (const auto label : labels) {
        const auto pos = text.find(label, search_from);
        if (pos != std::string::npos && (label_pos == std::string::npos || pos < label_pos)) {
            label_pos = pos;
            matched_label = label;
        }
    }
    if (label_pos == std::string::npos) return std::nullopt;

    auto payload_start = label_pos + matched_label.size();
    while (payload_start < text.size() && std::isspace(static_cast<unsigned char>(text[payload_start]))) ++payload_start;
    if (payload_start >= text.size() || (text[payload_start] != '{' && text[payload_start] != '[')) return std::nullopt;

    std::vector<char> stack;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = payload_start; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            stack.push_back('}');
        } else if (c == '[') {
            stack.push_back(']');
        } else if ((c == '}' || c == ']') && !stack.empty() && stack.back() == c) {
            stack.pop_back();
            if (stack.empty()) {
                const auto payload_end = i + 1;
                return std::make_pair(text.substr(payload_start, payload_end - payload_start),
                                      std::make_pair(label_pos, payload_end));
            }
        }
    }

    return std::nullopt;
}

static std::optional<std::pair<std::string, std::pair<std::size_t, std::size_t>>> FindJsonPayloadAt(
    const std::string& text,
    std::size_t payload_start) {
    while (payload_start < text.size() && std::isspace(static_cast<unsigned char>(text[payload_start]))) ++payload_start;
    if (payload_start >= text.size() || (text[payload_start] != '{' && text[payload_start] != '[')) return std::nullopt;

    std::vector<char> stack;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = payload_start; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }

        if (c == '"') in_string = true;
        else if (c == '{') stack.push_back('}');
        else if (c == '[') stack.push_back(']');
        else if ((c == '}' || c == ']') && !stack.empty() && stack.back() == c) {
            stack.pop_back();
            if (stack.empty()) {
                const auto payload_end = i + 1;
                return std::make_pair(text.substr(payload_start, payload_end - payload_start),
                                      std::make_pair(payload_start, payload_end));
            }
        }
    }

    return std::nullopt;
}

static std::string UnescapeLooseJsonString(std::string value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const char next = value[++i];
            if (next == 'n') output += '\n';
            else if (next == 'r') output += '\r';
            else if (next == 't') output += '\t';
            else output += next;
        } else {
            output += value[i];
        }
    }
    return TrimCopy(output);
}

static std::optional<std::string> ExtractLooseField(const std::string& text, std::string_view key) {
    const std::array<std::string, 3> patterns = {
        "\"" + std::string(key) + "\":",
        "\\\"" + std::string(key) + "\\\":",
        std::string(key) + ":"
    };

    std::size_t key_pos = std::string::npos;
    std::string matched;
    for (const auto& pattern : patterns) {
        const auto pos = text.find(pattern);
        if (pos != std::string::npos && (key_pos == std::string::npos || pos < key_pos)) {
            key_pos = pos;
            matched = pattern;
        }
    }
    if (key_pos == std::string::npos) return std::nullopt;

    auto value_start = key_pos + matched.size();
    while (value_start < text.size() && std::isspace(static_cast<unsigned char>(text[value_start]))) ++value_start;
    if (value_start >= text.size()) return std::nullopt;
    if (text.compare(value_start, 2, "\\\"") == 0) value_start += 2;
    else if (text[value_start] == '"') ++value_start;

    const std::array<std::string, 8> delimiters = {
        "\\\",\\\"description\\\"", "\",\"description\"",
        "\\\",\\\"name\\\"", "\",\"name\"",
        "\\\",\\\"arguments\\\"", "\",\"arguments\"",
        "\\\"}", "\"}"
    };
    std::size_t value_end = std::string::npos;
    for (const auto& delimiter : delimiters) {
        const auto pos = text.find(delimiter, value_start);
        if (pos != std::string::npos && (value_end == std::string::npos || pos < value_end)) {
            value_end = pos;
        }
    }
    if (value_end == std::string::npos) {
        value_end = text.find('\n', value_start);
        if (value_end == std::string::npos) value_end = text.size();
    }

    auto value = UnescapeLooseJsonString(text.substr(value_start, value_end - value_start));
    if (value.empty()) return std::nullopt;
    return value;
}

static nlohmann::json ExtractLooseCommandToolCall(const std::string& text,
                                                  std::size_t start_index,
                                                  const ToolNameRegistry& registry) {
    if (text.find("tool_calls:") == std::string::npos && text.find("tool_call:") == std::string::npos) {
        return nlohmann::json::array();
    }
    auto command = ExtractLooseField(text, "command");
    if (!command || command->empty()) return nlohmann::json::array();

    nlohmann::json arguments = {{"command", *command}};
    if (auto description = ExtractLooseField(text, "description")) {
        arguments["description"] = *description;
    }

    nlohmann::json raw_call = {
        {"id", MakeToolCallId(start_index)},
        {"type", "function"},
        {"function", {{"name", ResolveToolName(registry, "bash", arguments)}, {"arguments", arguments}}}
    };
    if (raw_call["function"]["name"].get<std::string>().empty()) return nlohmann::json::array();
    return NormalizeOpenAiToolCallsWithRegistry(nlohmann::json::array({raw_call}), registry);
}

static std::optional<std::pair<std::string, std::pair<std::size_t, std::size_t>>> FindHarmonyToolPayload(
    const std::string& text,
    std::size_t search_from) {
    const auto to_pos = text.find("to=", search_from);
    if (to_pos == std::string::npos) return std::nullopt;

    auto name_start = to_pos + 3;
    while (name_start < text.size() && std::isspace(static_cast<unsigned char>(text[name_start]))) ++name_start;
    auto name_end = name_start;
    while (name_end < text.size() && !std::isspace(static_cast<unsigned char>(text[name_end]))) ++name_end;
    if (name_end <= name_start) return std::nullopt;
    const auto requested_name = text.substr(name_start, name_end - name_start);
    const auto requested_base = BaseToolName(requested_name);
    if (requested_base.empty()) return std::nullopt;

    const auto json_marker = text.find("<|constrain|>json", name_end);
    if (json_marker == std::string::npos) return std::nullopt;
    auto payload_start = json_marker + std::string("<|constrain|>json").size();
    const auto message_marker = text.find("<|message|>", payload_start);
    if (message_marker != std::string::npos) {
        const auto first_json = text.find_first_of("{[", payload_start);
        if (first_json == std::string::npos || message_marker < first_json) {
            payload_start = message_marker + std::string("<|message|>").size();
        }
    }
    while (payload_start < text.size() && std::isspace(static_cast<unsigned char>(text[payload_start]))) ++payload_start;
    if (payload_start >= text.size() || (text[payload_start] != '{' && text[payload_start] != '[')) return std::nullopt;

    std::vector<char> stack;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = payload_start; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{') stack.push_back('}');
        else if (c == '[') stack.push_back(']');
        else if ((c == '}' || c == ']') && !stack.empty() && stack.back() == c) {
            stack.pop_back();
            if (stack.empty()) {
                auto payload_end = i + 1;
                auto span_end = payload_end;
                while (span_end < text.size() && std::isspace(static_cast<unsigned char>(text[span_end]))) ++span_end;
                if (text.compare(span_end, std::string("<|call|>").size(), "<|call|>") == 0) {
                    span_end += std::string("<|call|>").size();
                }
                return std::make_pair(requested_name + "\n" + text.substr(payload_start, payload_end - payload_start),
                                      std::make_pair(to_pos, span_end));
            }
        }
    }

    return std::nullopt;
}

struct ToolExtractionResult {
    nlohmann::json calls = nlohmann::json::array();
    std::string format = "none";
    std::string raw_preview;
    std::string error;
};

static bool LooksLikeNarratedToolIntent(const std::string& content) {
    auto text = LowerCopy(SanitizeAssistantContent(RemoveQwenToolCallBlocks(content)));
    text = TrimCopy(std::move(text));
    if (text.empty() || text.size() > 400 || text.size() < 20) return false;

    const bool has_future_action =
        text.find("i'll ") != std::string::npos ||
        text.find("i will ") != std::string::npos ||
        text.find("let me ") != std::string::npos ||
        text.find("i need to ") != std::string::npos ||
        text.find("i should ") != std::string::npos ||
        text.find("i'm going to ") != std::string::npos ||
        text.find("now let me ") != std::string::npos;

    const bool mentions_tool_protocol =
        text.find("tool call") != std::string::npos ||
        text.find("available tools") != std::string::npos ||
        text.find("use the ") != std::string::npos ||
        text.find("the `glob`") != std::string::npos ||
        text.find("the `read`") != std::string::npos ||
        text.find("the `grep`") != std::string::npos;

    const bool has_toolish_verb =
        text.find("read") != std::string::npos ||
        text.find("inspect") != std::string::npos ||
        text.find("explore") != std::string::npos ||
        text.find("search") != std::string::npos ||
        text.find("grep") != std::string::npos ||
        text.find("glob") != std::string::npos ||
        text.find("look at") != std::string::npos ||
        text.find("open ") != std::string::npos ||
        text.find("check ") != std::string::npos;

    const bool sounds_final =
        text.find("recommend") != std::string::npos ||
        text.find("summary") != std::string::npos ||
        text.find("findings") != std::string::npos ||
        text.find("conclusion") != std::string::npos ||
        text.find("overall") != std::string::npos;

    return has_future_action && (mentions_tool_protocol || has_toolish_verb) && !sounds_final;
}

static bool LooksLikeReasoningToolIntent(const std::string& reasoning) {
    auto text = LowerCopy(SanitizeAssistantContent(RemoveQwenToolCallBlocks(reasoning)));
    text = TrimCopy(std::move(text));
    if (text.empty()) return false;
    if (text.size() > 8000) text.resize(8000);

    const bool has_future_action =
        text.find("i'll ") != std::string::npos ||
        text.find("i will ") != std::string::npos ||
        text.find("let me ") != std::string::npos ||
        text.find("let's ") != std::string::npos ||
        text.find("i need to ") != std::string::npos ||
        text.find("i should ") != std::string::npos ||
        text.find("i can use") != std::string::npos ||
        text.find("i'm going to ") != std::string::npos ||
        text.find("start by ") != std::string::npos ||
        text.find("first, ") != std::string::npos ||
        text.find("next, ") != std::string::npos ||
        text.find("step 1") != std::string::npos;

    const bool mentions_tool_protocol =
        text.find("available tools") != std::string::npos ||
        text.find("tool call") != std::string::npos ||
        text.find("use `glob`") != std::string::npos ||
        text.find("use glob") != std::string::npos ||
        text.find("use `read`") != std::string::npos ||
        text.find("use read") != std::string::npos ||
        text.find("call glob") != std::string::npos ||
        text.find("call read") != std::string::npos;

    const bool has_toolish_verb =
        text.find("read") != std::string::npos ||
        text.find("inspect") != std::string::npos ||
        text.find("explore") != std::string::npos ||
        text.find("search") != std::string::npos ||
        text.find("grep") != std::string::npos ||
        text.find("glob") != std::string::npos ||
        text.find("look at") != std::string::npos ||
        text.find("open ") != std::string::npos ||
        text.find("check ") != std::string::npos;

    return has_future_action && (mentions_tool_protocol || has_toolish_verb);
}

static bool LooksLikeToolIntent(const std::string& text) {
    return text.find("<tool_call>") != std::string::npos ||
           text.find("assistant_tool_calls_json:") != std::string::npos ||
           text.find("tool_calls:") != std::string::npos ||
           text.find("tool_call:") != std::string::npos ||
           text.find("to=tool.") != std::string::npos ||
           text.find("to=tool_") != std::string::npos ||
           text.find("<|constrain|>json") != std::string::npos;
}

static std::size_t FlexibleFindTag(const std::string& text, const std::string& tag, std::size_t search_from) {
    std::string flexible;
    flexible.reserve(tag.size() + 4);
    flexible += '<';
    for (std::size_t i = 1; i < tag.size() - 1; ++i) {
        flexible += tag[i];
        if (tag[i] != ' ' && tag[i] != '_' && tag[i] != '-') {
            flexible += ' ';
        }
    }
    flexible += tag.back();
    auto pos = text.find(tag, search_from);
    if (pos != std::string::npos) return pos;
    pos = text.find(flexible, search_from);
    if (pos != std::string::npos) return pos;
    for (std::size_t i = search_from; i < text.size(); ++i) {
        if (text[i] == '<') {
            auto end_brace = text.find('>', i);
            if (end_brace == std::string::npos) break;
            auto inner = text.substr(i + 1, end_brace - i - 1);
            inner = TrimCopy(inner);
            if (inner == std::string(tag.begin() + 1, tag.end() - 1)) return i;
        }
    }
    return std::string::npos;
}

static std::optional<std::pair<std::string, std::size_t>> ExtractQwenXmlToolCall(const std::string& text, std::size_t search) {
    constexpr const char* open_tag = "<tool_call>";
    constexpr const char* close_tag = "</tool_call>";
    const auto start = FlexibleFindTag(text, open_tag, search);
    if (start == std::string::npos) return std::nullopt;
    const auto body_start = text.find('>', start);
    if (body_start == std::string::npos) return std::nullopt;
    const auto body_begin = body_start + 1;
    const auto end = text.find(close_tag, body_begin);
    if (end == std::string::npos) return std::nullopt;
    auto payload = TrimCopy(text.substr(body_begin, end - body_begin));
    if (payload.empty()) {
        return ExtractQwenXmlToolCall(text, end + std::string(close_tag).size());
    }
    return std::make_pair(payload, end + std::string(close_tag).size());
}

static nlohmann::json TryParseQwenXmlFunctionToolCall(const std::string& payload, std::size_t calls_size, const ToolNameRegistry& registry) {
    static constexpr const char* func_open = "<function=";
    auto fstart = payload.find(func_open);
    if (fstart == std::string::npos) return nlohmann::json::array();
    auto fname_start = fstart + std::string(func_open).size();
    auto fname_end = payload.find('>', fname_start);
    if (fname_end == std::string::npos) return nlohmann::json::array();
    std::string name = TrimCopy(payload.substr(fname_start, fname_end - fname_start));
    if (name.empty()) return nlohmann::json::array();

    static constexpr const char* func_close = "</function>";
    auto fclose = payload.find(func_close, fname_end);
    if (fclose == std::string::npos) return nlohmann::json::array();
    auto params_body = payload.substr(fname_end + 1, fclose - fname_end - 1);

    nlohmann::json args = nlohmann::json::object();
    static constexpr const char* param_open = "<parameter=";
    std::size_t psearch = 0;
    while (true) {
        auto pstart = params_body.find(param_open, psearch);
        if (pstart == std::string::npos) break;
        auto pname_start = pstart + std::string(param_open).size();
        auto pname_end = params_body.find('>', pname_start);
        if (pname_end == std::string::npos) break;
        std::string pname = TrimCopy(params_body.substr(pname_start, pname_end - pname_start));
        if (pname.empty()) { psearch = pname_end + 1; continue; }
        static constexpr const char* pclose_tag = "</parameter>";
        auto pclose = params_body.find(pclose_tag, pname_end);
        if (pclose == std::string::npos) break;
        auto pval_start = pname_end + 1;
        std::string pvalue = TrimCopy(params_body.substr(pval_start, pclose - pval_start));
        if (!pvalue.empty()) {
            try {
                auto parsed = nlohmann::json::parse(pvalue);
                args[pname] = parsed;
            } catch (...) {
                args[pname] = pvalue;
            }
        } else {
            args[pname] = "";
        }
        psearch = pclose + std::string(pclose_tag).size();
    }

    nlohmann::json result = nlohmann::json::object();
    result["id"] = "call_" + std::to_string(calls_size);
    result["type"] = "function";
    result["function"] = {{"name", name}, {"arguments", args.dump()}};
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back(result);
    return arr;
}

static nlohmann::json TryParseToolCallJson(const std::string& payload, std::size_t calls_size, const ToolNameRegistry& registry) {
    try {
        auto parsed = nlohmann::json::parse(payload);
        return NormalizeToolCallPayload(parsed, calls_size, registry);
    } catch (...) {
        return nlohmann::json::array();
    }
}

static nlohmann::json FindBareJsonToolCalls(const std::string& text, const ToolNameRegistry& registry) {
    const auto name_key = text.find("\"name\"");
    if (name_key == std::string::npos) return nlohmann::json::array();
    const auto args_key = text.find("\"arguments\"");
    if (args_key == std::string::npos) return nlohmann::json::array();
    auto json_start = text.find_first_of("{[", 0);
    while (json_start != std::string::npos) {
        auto found = FindJsonPayloadAt(text, json_start);
        if (!found) break;
        try {
            auto parsed = nlohmann::json::parse(found->first);
            if (parsed.is_object() && parsed.contains("name") && parsed.contains("arguments")) {
                auto normalized = NormalizeToolCallPayload(parsed, 0, registry);
                if (!normalized.empty()) return normalized;
            }
        } catch (...) {}
        json_start = text.find_first_of("{[", found->second.second);
    }
    return nlohmann::json::array();
}

static ToolExtractionResult ExtractToolCallsFromText(const std::string& text, const ToolNameRegistry& registry);

static void ExtractToolsFromThinkBlock(const std::string& text, nlohmann::json& calls, const ToolNameRegistry& registry, std::string& format) {
    std::size_t think_start = 0;
    while (true) {
        const auto open = text.find("<think>", think_start);
        if (open == std::string::npos) break;
        const auto close = text.find("</think>", open);
        if (close == std::string::npos) break;
        auto think_content = text.substr(open + 7, close - open - 7);
        auto inner_extraction = ExtractToolCallsFromText(think_content, registry);
        for (const auto& call : inner_extraction.calls) calls.push_back(call);
        if (!inner_extraction.calls.empty() && format == "none") format = "qwen_think_block";
        think_start = close + 8;
    }
}

static ToolExtractionResult ExtractToolCallsFromText(const std::string& text, const ToolNameRegistry& registry) {
    ToolExtractionResult result;
    nlohmann::json calls = nlohmann::json::array();

    inferdeck::core::Logger::Get().Debug("ExtractToolCallsFromText input: " + SafeLogText(text, 2048));

    std::size_t search = 0;
    while (true) {
        auto found = ExtractQwenXmlToolCall(text, search);
        if (!found) break;
        search = found->second;
        auto normalized = TryParseToolCallJson(found->first, calls.size(), registry);
        if (normalized.empty()) {
            normalized = TryParseQwenXmlFunctionToolCall(found->first, calls.size(), registry);
        }
        for (const auto& call : normalized) calls.push_back(call);
        if (!normalized.empty() && result.format == "none") result.format = "qwen_xml";
    }

    search = 0;
    while (auto found = FindLabeledJsonPayload(text, search)) {
        search = found->second.second;
        try {
            auto parsed = nlohmann::json::parse(found->first);
            auto normalized = NormalizeToolCallPayload(parsed, calls.size(), registry);
            for (const auto& call : normalized) calls.push_back(call);
            if (!normalized.empty() && result.format == "none") result.format = "raw_tool_calls";
        } catch (...) {
            continue;
        }

        std::size_t sibling_search = found->second.second;
        while (sibling_search < text.size()) {
            while (sibling_search < text.size() && std::isspace(static_cast<unsigned char>(text[sibling_search]))) ++sibling_search;
            if (sibling_search >= text.size() || text[sibling_search] != ',') break;
            ++sibling_search;
            auto sibling = FindJsonPayloadAt(text, sibling_search);
            if (!sibling) break;
            try {
                auto parsed = nlohmann::json::parse(sibling->first);
                auto normalized = NormalizeToolCallPayload(parsed, calls.size(), registry);
                for (const auto& call : normalized) calls.push_back(call);
                if (!normalized.empty() && result.format == "none") result.format = "raw_tool_calls";
                sibling_search = sibling->second.second;
                search = sibling_search;
            } catch (...) {
                break;
            }
        }
    }

    search = 0;
    while (auto found = FindHarmonyToolPayload(text, search)) {
        search = found->second.second;
        const auto separator = found->first.find('\n');
        if (separator == std::string::npos) continue;
        const auto requested_name = found->first.substr(0, separator);
        const auto payload = found->first.substr(separator + 1);
        try {
            auto parsed = nlohmann::json::parse(payload);
            auto normalized = NormalizeToolCallPayload(parsed, calls.size(), registry, requested_name);
            for (const auto& call : normalized) calls.push_back(call);
            if (!normalized.empty() && result.format == "none") result.format = "gpt_oss_harmony";
        } catch (...) {
            continue;
        }
    }

    if (calls.empty()) {
        ExtractToolsFromThinkBlock(text, calls, registry, result.format);
    }

    if (calls.empty()) {
        auto bare = FindBareJsonToolCalls(text, registry);
        for (const auto& call : bare) calls.push_back(call);
        if (!bare.empty() && result.format == "none") result.format = "bare_json";
    }

    if (calls.empty()) {
        auto loose = ExtractLooseCommandToolCall(text, calls.size(), registry);
        for (const auto& call : loose) calls.push_back(call);
        if (!loose.empty() && result.format == "none") result.format = "raw_tool_calls";
    }
    if (!calls.empty()) {
        result.calls = std::move(calls);
    } else if (LooksLikeToolIntent(text)) {
        result.error = "tool intent was present but no valid advertised tool call could be extracted";
    } else if (registry.has_tools && LooksLikeNarratedToolIntent(text)) {
        result.format = "narrated_intent";
        result.error = "assistant narrated a future tool action instead of emitting a structured tool call";
    }
    if (LooksLikeToolIntent(text) || result.format == "narrated_intent") {
        result.raw_preview = SafeLogText(text, 240);
    }
    return result;
}

static std::string RemoveQwenToolCallBlocks(std::string text) {
    while (true) {
        const auto start = text.find("<tool_call>");
        if (start == std::string::npos) break;
        const auto end = text.find("</tool_call>", start);
        if (end == std::string::npos) {
            text.erase(start);
            break;
        }
        text.erase(start, end + std::string("</tool_call>").size() - start);
    }
    std::size_t search = 0;
    while (auto found = FindLabeledJsonPayload(text, search)) {
        std::size_t erase_end = found->second.second;
        std::size_t sibling_search = erase_end;
        while (sibling_search < text.size()) {
            while (sibling_search < text.size() && std::isspace(static_cast<unsigned char>(text[sibling_search]))) ++sibling_search;
            if (sibling_search >= text.size() || text[sibling_search] != ',') break;
            auto sibling = FindJsonPayloadAt(text, sibling_search + 1);
            if (!sibling) break;
            erase_end = sibling->second.second;
            sibling_search = erase_end;
        }
        text.erase(found->second.first, erase_end - found->second.first);
        search = found->second.first;
    }
    search = 0;
    while (auto found = FindHarmonyToolPayload(text, search)) {
        text.erase(found->second.first, found->second.second - found->second.first);
        search = found->second.first;
    }
    if (!ExtractLooseCommandToolCall(text, 0, ToolNameRegistry{}).empty()) {
        auto pos = text.find("tool_calls:");
        if (pos == std::string::npos) pos = text.find("tool_call:");
        if (pos != std::string::npos) text.erase(pos);
    }
    return TrimCopy(text);
}

std::string ValidateChatRequest(const std::string& body) {
    if (body.empty()) return "Request body is required";

    try {
        auto parsed = json::parse(body);
        if (!parsed.contains("messages") || !parsed["messages"].is_array()) {
            return "Missing or invalid messages field";
        }

        for (const auto& message : parsed["messages"]) {
            if (!message.contains("role") || !message["role"].is_string()) {
                return "Each message requires a role";
            }
            std::string role = message["role"].get<std::string>();
            if (role != "system" && role != "user" && role != "assistant" && role != "tool") {
                return "Invalid message role";
            }
        }
    } catch (const std::exception& e) {
        return e.what();
    }

    return "";
}

bool ShouldForceNonStreamingBackend(const nlohmann::json& request) {
    return request.contains("tools") && request["tools"].is_array() && !request["tools"].empty();
}

static std::string SafeLogText(std::string text, std::size_t limit = 120) {
    for (auto& c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc == 127) c = ' ';
    }
    if (text.size() > limit) text.resize(limit);
    return text;
}

bool ShouldUseStreamingBackend(const nlohmann::json& request) {
    return request.value("stream", false) && !ShouldForceNonStreamingBackend(request);
}

bool ShouldUseStreamingBackendForClient(const nlohmann::json& request, const std::string& client) {
    (void)client;
    return request.value("stream", false) &&
           !ShouldForceNonStreamingBackend(request);
}

static void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = text.find(from.data(), pos, from.size())) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string TrimCopy(std::string text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

static void AppendReasoning(std::string& reasoning, const std::string& text) {
    auto clean = TrimCopy(text);
    if (clean.empty()) return;
    if (!reasoning.empty()) reasoning += "\n";
    reasoning += clean;
}

static std::string CompactToolResultContent(const std::string& content) {
    constexpr std::size_t kMaxToolContentChars = 6000;
    constexpr std::size_t kHeadChars = 4200;
    constexpr std::size_t kTailChars = 1200;
    if (content.size() <= kMaxToolContentChars) return content;

    std::string compact;
    compact.reserve(kHeadChars + kTailChars + 512);
    compact += content.substr(0, kHeadChars);
    compact += "\n\n<inferdeck_tool_result_truncated original_chars=\"";
    compact += std::to_string(content.size());
    compact += "\" preserved=\"head_and_tail\" reason=\"tool output too large for reliable continuation\" />\n\n";
    compact += content.substr(content.size() - kTailChars);
    return compact;
}

static std::string CombinedReasoningText(const inferdeck::core::InferenceResult& result) {
    std::string reasoning = result.reasoning_text;
    AppendReasoning(reasoning, ExtractAssistantReasoningContent(RemoveQwenToolCallBlocks(result.text)));
    return reasoning;
}

static ToolExtractionResult AnalyzeInferenceToolExtraction(const inferdeck::core::InferenceResult& result,
                                                           const ToolNameRegistry& registry) {
    auto extraction = ExtractToolCallsFromText(result.text, registry);
    const auto reasoning = CombinedReasoningText(result);
    if (extraction.calls.empty() && extraction.error.empty() && registry.has_tools &&
        LooksLikeReasoningToolIntent(reasoning)) {
        extraction.format = "narrated_intent";
        extraction.error = "assistant narrated a future tool action in reasoning instead of emitting a structured tool call";
        extraction.raw_preview = SafeLogText(reasoning, 240);
    }
    return extraction;
}

static bool ShouldAttemptToolIntentRepair(const inferdeck::core::InferenceResult& result,
                                          const ToolNameRegistry& registry) {
    if (result.HasError() || !registry.has_tools) return false;
    const auto extraction = AnalyzeInferenceToolExtraction(result, registry);
    return extraction.calls.empty() && extraction.format == "narrated_intent";
}

static std::string ToolNamesSummary(const ToolNameRegistry& registry) {
    std::string names;
    for (const auto& name : registry.names) {
        if (!names.empty()) names += ", ";
        names += name;
    }
    return names;
}

static std::string RepairPreviewText(const inferdeck::core::InferenceResult& result) {
    std::string text;
    const auto reasoning = CombinedReasoningText(result);
    if (!reasoning.empty()) {
        text += "Reasoning:\n";
        text += reasoning.substr(0, 800);
        text += "\n\n";
    }
    if (!result.text.empty()) {
        text += "Assistant content:\n";
        text += result.text.substr(0, 800);
    }
    return text.empty() ? "(empty assistant response)" : text;
}

static std::string BuildToolInstructionForRepair(const ToolNameRegistry& registry) {
    auto family = inferdeck::core::LlamaEngine::Get().GetModelFamily();
    auto names = ToolNamesSummary(registry);
    if (family == "qwen" || family == "universal") {
        return
            "You must emit exactly one tool call in this format:\n"
            "<tool_call>{\"name\":\"" + (names.empty() ? std::string("tool_name") : names.substr(0, names.find(','))) + "\",\"arguments\":{...}}</tool_call>\n"
            "Do not include any other text. Available tools: " + names;
    }
    return
        "You must emit exactly one tool call in this format:\n"
        "<|channel|>commentary to=" + (names.empty() ? std::string("tool_name") : names.substr(0, names.find(','))) + " <|constrain|>json<|message|>{...}<|call|>\n"
        "Do not include any other text. Available tools: " + names;
}

static std::vector<inferdeck::core::ChatMessage> BuildToolIntentRepairMessages(
    const std::vector<inferdeck::core::ChatMessage>& messages,
    const inferdeck::core::InferenceResult& invalid_result,
    const ToolNameRegistry& registry) {
    auto repair_messages = messages;
    std::string prompt =
        "The previous assistant response was invalid for this tool-capable OpenCode request. "
        "It narrated future tool use instead of emitting a structured tool call.\n\n"
        "Available tool names: " + ToolNamesSummary(registry) + "\n\n"
        "Invalid previous assistant response:\n" + RepairPreviewText(invalid_result) + "\n\n"
        "Repair now. If a tool is needed, emit exactly one machine-readable tool call and no prose. "
        + BuildToolInstructionForRepair(registry) + " "
        "If no tool is needed, provide the final answer only.";
    repair_messages.push_back({inferdeck::core::MessageRole::User, prompt, "", "", ""});
    return repair_messages;
}

static inferdeck::core::InferenceParams BuildToolIntentRepairParams(inferdeck::core::InferenceParams params) {
    params.max_tokens = 1024;
    params.temperature = 0.0f;
    params.top_p = 1.0f;
    return params;
}

static bool HasStructuredToolCall(const inferdeck::core::InferenceResult& result,
                                  const ToolNameRegistry& registry) {
    if (!result.tool_calls.empty()) return true;
    return !ExtractToolCallsFromText(result.text, registry).calls.empty();
}

std::string ExtractAssistantReasoningContent(const std::string& content) {
    std::string text = content;
    std::string reasoning;

    while (true) {
        const auto start = text.find("<think>");
        if (start == std::string::npos) break;
        const auto content_start = start + std::string("<think>").size();
        const auto end = text.find("</think>", content_start);
        if (end == std::string::npos) {
            AppendReasoning(reasoning, text.substr(content_start));
            break;
        }
        AppendReasoning(reasoning, text.substr(content_start, end - content_start));
        text.erase(start, end + std::string("</think>").size() - start);
    }

    while (true) {
        const auto start = text.find("<|channel|>analysis<|message|>");
        if (start == std::string::npos) break;
        const auto content_start = start + std::string("<|channel|>analysis<|message|>").size();
        const auto end = text.find("<|end|>", content_start);
        if (end == std::string::npos) {
            AppendReasoning(reasoning, text.substr(content_start));
            break;
        }
        AppendReasoning(reasoning, text.substr(content_start, end - content_start));
        text.erase(start, end + std::string("<|end|>").size() - start);
    }

    return reasoning;
}

std::string SanitizeAssistantContent(const std::string& content) {
    std::string text = content;

    while (true) {
        const auto start = text.find("<think>");
        if (start == std::string::npos) break;
        const auto end = text.find("</think>", start);
        if (end == std::string::npos) {
            text.erase(start);
            break;
        }
        text.erase(start, end + std::string("</think>").size() - start);
    }

    while (true) {
        const auto channel = text.find("<|channel|>analysis<|message|>");
        if (channel == std::string::npos) break;
        const auto end = text.find("<|end|>", channel);
        if (end == std::string::npos) {
            text.erase(channel);
            break;
        }
        text.erase(channel, end + std::string("<|end|>").size() - channel);
    }

    ReplaceAll(text, "<|start|>assistant", "");
    ReplaceAll(text, "<|start|>system", "");
    ReplaceAll(text, "<|start|>user", "");
    ReplaceAll(text, "<|channel|>final<|message|>", "");
    ReplaceAll(text, "<|channel|>commentary<|message|>", "");
    ReplaceAll(text, "<|channel|>analysis", "");
    ReplaceAll(text, "<|channel|>final", "");
    ReplaceAll(text, "<|channel|>commentary", "");
    ReplaceAll(text, "<|message|>", "");
    ReplaceAll(text, "<|start|>", "");
    ReplaceAll(text, "<|channel|>", "");
    ReplaceAll(text, "<|end|>", "");
    return TrimCopy(text);
}

static std::string BuildSyntheticChatCompletionStreamWithRegistry(const nlohmann::json& response,
                                                                  const ToolNameRegistry& registry) {
    std::string id = response.value("id", MakeId());
    std::string model = response.value("model", "local-model");
    std::string stream;

    stream += SseDeltaChunk(id, model, json({{"role", "assistant"}}));

    std::string finish_reason = "stop";
    if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty()) {
        const auto& choice = response["choices"][0];
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
            finish_reason = choice["finish_reason"].get<std::string>();
        }

        if (choice.contains("message") && choice["message"].is_object()) {
            const auto& message = choice["message"];
            std::string reasoning_content;
            json extracted_tool_calls = json::array();
            if (message.contains("reasoning_content") && message["reasoning_content"].is_string()) {
                AppendReasoning(reasoning_content, message["reasoning_content"].get<std::string>());
            }
            if (message.contains("content") && message["content"].is_string() && !message["content"].get<std::string>().empty()) {
                const auto raw_content = message["content"].get<std::string>();
                extracted_tool_calls = ExtractToolCallsFromText(raw_content, registry).calls;
                const auto without_tool_calls = RemoveQwenToolCallBlocks(raw_content);
                AppendReasoning(reasoning_content, ExtractAssistantReasoningContent(without_tool_calls));
                const auto clean_content = SanitizeAssistantContent(without_tool_calls);
                if (!reasoning_content.empty()) {
                    stream += SseDeltaChunk(id, model, json({{"reasoning_content", reasoning_content}}));
                }
                if (!clean_content.empty()) {
                    stream += SseDeltaChunk(id, model, json({{"content", clean_content}}));
                }
            } else if (!reasoning_content.empty()) {
                stream += SseDeltaChunk(id, model, json({{"reasoning_content", reasoning_content}}));
            }
            auto tool_calls = NormalizeOpenAiToolCallsWithRegistry(message.contains("tool_calls") ? message["tool_calls"] : json::array(), registry);
            for (const auto& tool_call : extracted_tool_calls) tool_calls.push_back(tool_call);
            if (!tool_calls.empty()) {
                for (size_t i = 0; i < tool_calls.size(); ++i) {
                    json tool_call = tool_calls[i];
                    tool_call["index"] = i;
                    stream += SseDeltaChunk(id, model, json({{"tool_calls", json::array({tool_call})}}));
                }
            }
        }
    }

    stream += SseDeltaChunk(id, model, json::object(), finish_reason);
    stream += "data: [DONE]\n\n";
    return stream;
}

std::string BuildSyntheticChatCompletionStream(const nlohmann::json& response) {
    return BuildSyntheticChatCompletionStreamWithRegistry(response, ToolNameRegistry{});
}

std::string BuildSyntheticChatCompletionStream(const nlohmann::json& response, const nlohmann::json& request_tools) {
    return BuildSyntheticChatCompletionStreamWithRegistry(response, BuildToolNameRegistry(request_tools));
}

static std::string SseChunk(const std::string& id, const std::string& model, const std::string& content, const std::string& reasoning_content, bool done) {
    json chunk;
    chunk["id"] = id;
    chunk["object"] = "chat.completion.chunk";
    chunk["model"] = model;
    chunk["created"] = std::time(nullptr);
    if (done) {
        chunk["choices"] = json::array({{{"index", 0}, {"delta", json::object()}, {"finish_reason", "stop"}}});
    } else {
        json delta = {{"role", "assistant"}};
        if (!content.empty()) {
            delta["content"] = content;
        }
        if (!reasoning_content.empty()) {
            delta["reasoning_content"] = reasoning_content;
        }
        chunk["choices"] = json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", nullptr}}});
    }
    return "data: " + chunk.dump() + "\n\n";
}

static std::string SseDeltaChunk(const std::string& id, const std::string& model, const json& delta, const json& finish_reason) {
    json chunk;
    chunk["id"] = id;
    chunk["object"] = "chat.completion.chunk";
    chunk["model"] = model;
    chunk["created"] = std::time(nullptr);
    chunk["choices"] = json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish_reason}}});
    return "data: " + chunk.dump() + "\n\n";
}

static std::string SseErrorChunk(const std::string& id, const std::string& model, const std::string& message) {
    json chunk;
    chunk["id"] = id;
    chunk["object"] = "chat.completion.chunk";
    chunk["model"] = model;
    chunk["created"] = std::time(nullptr);
    chunk["choices"] = json::array({{
        {"index", 0},
        {"delta", json::object()},
        {"finish_reason", "error"}
    }});
    chunk["error"] = {{"message", message}, {"type", "backend_error"}, {"code", "backend_stream_error"}};
    return "data: " + chunk.dump() + "\n\n";
}

static json BuildChatCompletionResponse(const std::string& id,
                                        const std::string& model,
                                        const inferdeck::core::InferenceResult& result,
                                        const ToolNameRegistry& registry = ToolNameRegistry{}) {
    json response;
    response["id"] = id;
    response["object"] = "chat.completion";
    response["created"] = std::time(nullptr);
    response["model"] = model;

    json message = {{"role", "assistant"}};
    auto extracted = AnalyzeInferenceToolExtraction(result, registry);
    auto qwen_tool_calls = extracted.calls;
    const auto visible_text = RemoveQwenToolCallBlocks(result.text);
    if (!visible_text.empty()) message["content"] = SanitizeAssistantContent(visible_text);
    std::string reasoning_text = result.reasoning_text;
    AppendReasoning(reasoning_text, ExtractAssistantReasoningContent(visible_text));
    if (!reasoning_text.empty()) {
        message["reasoning_content"] = reasoning_text;
    }
    json tool_calls = json::array();
    if (!result.tool_calls.empty()) {
        for (const auto& tc : result.tool_calls) {
            json tc_json;
            tc_json["id"] = tc.id;
            tc_json["type"] = tc.type;
            tc_json["function"] = {{"name", tc.function_name}, {"arguments", JsonArgumentString(tc.function_arguments)}};
            tool_calls.push_back(tc_json);
        }
    }
    for (const auto& tc : qwen_tool_calls) tool_calls.push_back(tc);
    tool_calls = NormalizeOpenAiToolCallsWithRegistry(tool_calls, registry);
    if (!tool_calls.empty()) message["tool_calls"] = tool_calls;
    if (!message.contains("content") && tool_calls.empty()) message["content"] = "";

    std::string finish_reason = tool_calls.empty() ? "stop" : "tool_calls";
    if (tool_calls.empty() && registry.has_tools && !extracted.error.empty()) {
        response["_tool_extraction"] = {
            {"error", extracted.error},
            {"format", extracted.format},
            {"raw_preview", extracted.raw_preview}
        };
        if (!visible_text.empty()) {
            message["content"] = SanitizeAssistantContent(visible_text);
        } else {
            message["content"] = "(I attempted to use a tool but encountered an issue. Please rephrase your request.)";
        }
        finish_reason = "stop";
    }
    response["choices"] = json::array({{{"index", 0}, {"message", message}, {"finish_reason", finish_reason}}});
    response["usage"] = {{"prompt_tokens", result.prompt_tokens}, {"completion_tokens", result.completion_tokens}, {"total_tokens", result.total_tokens}};
    return response;
}

nlohmann::json BuildChatCompletionResponseForTest(const std::string& id,
                                                  const std::string& model,
                                                  const std::string& content,
                                                  const nlohmann::json& request_tools) {
    inferdeck::core::InferenceResult result;
    result.text = content;
    result.completion_tokens = static_cast<int>(content.size());
    result.total_tokens = result.completion_tokens;
    return BuildChatCompletionResponse(id, model, result, BuildToolNameRegistry(request_tools));
}

nlohmann::json BuildChatCompletionResponseForTest(const std::string& id,
                                                  const std::string& model,
                                                  const std::string& content,
                                                  const std::string& reasoning,
                                                  const nlohmann::json& request_tools) {
    inferdeck::core::InferenceResult result;
    result.text = content;
    result.reasoning_text = reasoning;
    result.completion_tokens = static_cast<int>(content.size() + reasoning.size());
    result.total_tokens = result.completion_tokens;
    return BuildChatCompletionResponse(id, model, result, BuildToolNameRegistry(request_tools));
}

std::string CompactToolResultContentForTest(const std::string& content) {
    return CompactToolResultContent(content);
}

std::string DetectChatClientName(const httplib::Request& req) {
    std::string explicit_client = req.get_header_value("X-InferDeck-Client");
    if (!explicit_client.empty()) return explicit_client.substr(0, 80);
    std::string user_agent = req.get_header_value("User-Agent");
    std::string lower = user_agent;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("open-webui") != std::string::npos) return "Open WebUI";
    if (lower.find("opencode") != std::string::npos) return "OpenCode";
    if (lower.find("ollama") != std::string::npos) return "Ollama compatible";
    if (!user_agent.empty()) return user_agent.substr(0, 80);
    return "OpenAI compatible";
}

static json BuildJobPayloadPreview(const json& body, bool stream, bool force_non_streaming_backend, const std::string& request_protocol) {
    json preview;
    preview["requestProtocol"] = request_protocol;
    preview["model"] = body.value("model", "");
    preview["stream"] = stream;
    preview["tools"] = body.contains("tools") && body["tools"].is_array() ? body["tools"].size() : 0;
    preview["messages"] = body.contains("messages") && body["messages"].is_array() ? body["messages"].size() : 0;
    preview["forcedNonStreamingBackend"] = force_non_streaming_backend;
    if (body.contains("max_tokens")) preview["maxTokens"] = body["max_tokens"];
    if (body.contains("temperature")) preview["temperature"] = body["temperature"];
    return preview;
}

static void SetInferenceError(httplib::Response& resp, const inferdeck::core::InferenceResult& result) {
    int status = result.http_status >= 400 ? result.http_status : 502;
    resp.status = status;
    resp.set_content(json({{"error", {{"message", result.error_message.empty() ? "Backend inference failed" : result.error_message},
                                      {"type", "backend_error"},
                                      {"code", status}}}}).dump(), "application/json");
}

struct StreamQueue {
    std::queue<std::string> chunks;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> done{false};

    void push(std::string chunk) {
        std::lock_guard<std::mutex> lock(mtx);
        chunks.push(std::move(chunk));
        cv.notify_one();
    }

    bool pop(std::string& out) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return !chunks.empty() || done.load(); });
        if (chunks.empty()) return false;
        out = std::move(chunks.front());
        chunks.pop();
        return true;
    }

    bool pop_for(std::string& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, timeout, [this] { return !chunks.empty() || done.load(); });
        if (chunks.empty()) return false;
        out = std::move(chunks.front());
        chunks.pop();
        return true;
    }

    void finish() {
        done.store(true);
        cv.notify_all();
    }
};

struct StreamState {
    std::shared_ptr<StreamQueue> queue = std::make_shared<StreamQueue>();
    std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::atomic<bool> abandoned{false};
    std::atomic<bool> abort_requested{false};
    std::mutex mtx;

    void touch() {
        std::lock_guard<std::mutex> lock(mtx);
        last_activity = std::chrono::steady_clock::now();
    }

    std::chrono::steady_clock::time_point last() {
        std::lock_guard<std::mutex> lock(mtx);
        return last_activity;
    }

    bool request_abort() {
        bool expected = false;
        return abort_requested.compare_exchange_strong(expected, true);
    }
};

static int EnvIntMs(const char* name, int fallback_ms) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback_ms;
    try {
        int parsed = std::stoi(value);
        return parsed > 0 ? parsed : fallback_ms;
    } catch (...) {
        return fallback_ms;
    }
}

struct GuardedPredictState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    inferdeck::core::InferenceResult result;
    std::string exception;
};

static nlohmann::json SyntheticResultPreview(const nlohmann::json& response,
                                             const inferdeck::core::InferenceResult& result,
                                             const std::string& request_protocol,
                                             const std::string& response_mode,
                                             std::uint64_t sse_chunks,
                                             std::uint64_t heartbeat_chunks,
                                             std::uint64_t response_bytes,
                                             const ToolNameRegistry& registry,
                                             const std::string& cause = "") {
    auto extraction = AnalyzeInferenceToolExtraction(result, registry);
    nlohmann::json preview = {
        {"requestProtocol", request_protocol},
        {"responseProtocol", "openai.sse"},
        {"responseMode", response_mode},
        {"sseChunks", sse_chunks},
        {"heartbeatChunks", heartbeat_chunks},
        {"responseBytes", response_bytes},
        {"finishReason", ResponseFinishReason(response)},
        {"toolCallCount", ResponseToolCallCount(response)},
        {"rawToolTextPreview", extraction.raw_preview},
        {"extractedToolCallCount", extraction.calls.size()},
        {"toolExtractionFormat", extraction.format},
        {"toolExtractionError", extraction.error},
        {"contentPreview", result.text.substr(0, 500)},
        {"reasoningPreview", result.reasoning_text.substr(0, 500)},
        {"waitingOnBackendOrToolFormatting", false},
        {"backendAbortState", "none"},
        {"syntheticStream", true}
    };
    if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty() &&
        response["choices"][0].contains("message")) {
        const auto& message = response["choices"][0]["message"];
        preview["toolCalls"] = message.contains("tool_calls") ? message["tool_calls"] : nlohmann::json::array();
    } else {
        preview["toolCalls"] = nlohmann::json::array();
    }
    if (response.contains("usage")) preview["usage"] = response["usage"];
    if (!cause.empty()) preview["terminalCause"] = cause;
    return preview;
}

/*
static bool RunGuardedSyntheticSse(inferdeck::core::LlamaEngine& engine,
                                   const std::vector<inferdeck::core::ChatMessage>& messages,
                                   const inferdeck::core::InferenceParams& params,
                                   const std::string& id,
                                   const std::string& response_model_id,
                                   const std::string& job_id,
                                   const std::string& log_label,
                                   const std::string& request_protocol,
                                   const ToolNameRegistry& tool_registry,
                                   httplib::DataSink& sink) {
    constexpr int kDefaultHeartbeatMs = 5000;
    constexpr int kDefaultIdleTimeoutMs = 300000;
    constexpr int kDefaultTotalTimeoutMs = 600000;

    const auto heartbeat_interval = std::chrono::milliseconds(EnvIntMs("INFERDECK_OPENCODE_HEARTBEAT_MS", kDefaultHeartbeatMs));
    const auto idle_timeout = std::chrono::milliseconds(EnvIntMs("INFERDECK_OPENCODE_IDLE_TIMEOUT_MS", kDefaultIdleTimeoutMs));
    const auto total_timeout = std::chrono::milliseconds(EnvIntMs("INFERDECK_OPENCODE_TOTAL_TIMEOUT_MS", kDefaultTotalTimeoutMs));

    auto state = std::make_shared<GuardedPredictState>();
    auto terminal_sent = std::make_shared<std::atomic<bool>>(false);
    std::uint64_t sse_chunks_sent = 0;
    std::uint64_t sse_bytes_sent = 0;
    std::uint64_t heartbeat_chunks_sent = 0;

    auto mark_failed_once = [&](const std::string& cause, int status_code) {
        bool expected = false;
        if (!terminal_sent->compare_exchange_strong(expected, true)) return;
        inferdeck::gateway::RuntimeActivity::Get().FailJob(job_id, cause, status_code);
    };

    auto write_chunk = [&](const std::string& chunk) {
        sse_chunks_sent += CountSseDataChunks(chunk);
        sse_bytes_sent += chunk.size();
        return sink.write(chunk.data(), chunk.size());
    };

    std::thread([state, &engine, messages, params]() {
        inferdeck::core::InferenceResult result;
        std::string exception;
        try {
            std::unique_lock<std::mutex> backend_lock(g_switch_mutex);
            result = engine.Predict(messages, params);
        } catch (const std::exception& e) {
            exception = e.what();
        } catch (...) {
            exception = "Unknown backend inference failure";
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->result = std::move(result);
            state->exception = std::move(exception);
            state->done = true;
        }
        state->cv.notify_all();
    }).detach();

    const auto start = std::chrono::steady_clock::now();
    auto last_backend_activity = start;
    auto next_heartbeat = start;

    inferdeck::core::Logger::Get().Info("Request " + job_id + " using guarded synthetic SSE for " + log_label);

    while (true) {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->cv.wait_until(lock, next_heartbeat, [&] { return state->done; })) {
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - start >= total_timeout) {
            const std::string cause = "OpenCode " + log_label + " totalTimeout";
            inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, nlohmann::json({
                {"requestProtocol", request_protocol},
                {"responseProtocol", "openai.sse"},
                {"responseMode", "guarded-synthetic-sse-error"},
                {"finishReason", "error"},
                {"terminalCause", "timeout"},
                {"backendAbortState", "aborted"},
                {"waitingOnBackendOrToolFormatting", false}
            }), "Guarded synthetic SSE timed out");
            mark_failed_once(cause, 504);
            inferdeck::gateway::RuntimeActivity::Get().FailRunningClientJobs("OpenCode", "backendAborted: " + cause, 504, job_id);
            inferdeck::core::LlamaEngine::Get().AbortActiveRequest(cause);
            const auto error_stream = SseErrorChunk(id, response_model_id, cause) +
                                      SseDeltaChunk(id, response_model_id, nlohmann::json::object(), "error") +
                                      "data: [DONE]\n\n";
            write_chunk(error_stream);
            sink.done();
            return false;
        }

        if (now - last_backend_activity >= idle_timeout) {
            const std::string cause = "OpenCode " + log_label + " idleTimeout";
            inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, nlohmann::json({
                {"requestProtocol", request_protocol},
                {"responseProtocol", "openai.sse"},
                {"responseMode", "guarded-synthetic-sse-error"},
                {"finishReason", "error"},
                {"terminalCause", "idle-timeout"},
                {"backendAbortState", "aborted"},
                {"waitingOnBackendOrToolFormatting", false}
            }), "Guarded synthetic SSE idle timed out");
            mark_failed_once(cause, 504);
            inferdeck::gateway::RuntimeActivity::Get().FailRunningClientJobs("OpenCode", "backendAborted: " + cause, 504, job_id);
            inferdeck::core::LlamaEngine::Get().AbortActiveRequest(cause);
            const auto error_stream = SseErrorChunk(id, response_model_id, cause) +
                                      SseDeltaChunk(id, response_model_id, nlohmann::json::object(), "error") +
                                      "data: [DONE]\n\n";
            write_chunk(error_stream);
            sink.done();
            return false;
        }

        const std::string heartbeat = ": inferdeck opencode " + log_label + " backend running\n\n";
        if (!write_chunk(heartbeat)) {
            const std::string cause = "OpenCode " + log_label + " clientDisconnected";
            inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, nlohmann::json({
                {"requestProtocol", request_protocol},
                {"responseProtocol", "openai.sse"},
                {"responseMode", "guarded-synthetic-sse-error"},
                {"finishReason", "error"},
                {"terminalCause", "client-disconnect"},
                {"backendAbortState", "aborted"},
                {"waitingOnBackendOrToolFormatting", false}
            }), "Guarded synthetic SSE client disconnected");
            mark_failed_once(cause, 499);
            inferdeck::gateway::RuntimeActivity::Get().FailRunningClientJobs("OpenCode", "backendAborted: " + cause, 499, job_id);
            inferdeck::core::LlamaEngine::Get().AbortActiveRequest(cause);
            sink.done();
            return false;
        }
        ++heartbeat_chunks_sent;
        last_backend_activity = now;
        next_heartbeat = now + heartbeat_interval;
    }

    inferdeck::core::InferenceResult result;
    std::string exception;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        result = state->result;
        exception = state->exception;
    }

    if (!exception.empty()) {
        const auto error_stream = SseErrorChunk(id, response_model_id, exception) +
                                  SseDeltaChunk(id, response_model_id, nlohmann::json::object(), "error") +
                                  "data: [DONE]\n\n";
        inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, nlohmann::json({
            {"requestProtocol", request_protocol},
            {"responseProtocol", "openai.sse"},
            {"responseMode", "guarded-synthetic-sse-error"},
            {"finishReason", "error"},
            {"terminalCause", "backend-exception"},
            {"backendAbortState", "exception"},
            {"waitingOnBackendOrToolFormatting", false}
        }), "Guarded synthetic SSE backend exception");
        mark_failed_once(exception, 500);
        write_chunk(error_stream);
        sink.done();
        return false;
    }

    if (result.HasError()) {
        const auto error_stream = SseErrorChunk(id, response_model_id, result.error_message) +
                                  SseDeltaChunk(id, response_model_id, nlohmann::json::object(), "error") +
                                  "data: [DONE]\n\n";
        bool expected = false;
        if (terminal_sent->compare_exchange_strong(expected, true)) {
            inferdeck::gateway::RuntimeActivity::Get().CompleteJob(job_id, result, nlohmann::json({
                {"responseMode", "guarded-synthetic-sse-error"},
                {"requestProtocol", request_protocol},
                {"responseProtocol", "openai.sse"},
                {"sseChunks", CountSseDataChunks(error_stream)},
                {"heartbeatChunks", heartbeat_chunks_sent},
                {"responseBytes", error_stream.size()},
                {"finishReason", "error"},
                {"toolCallCount", 0},
                {"error", result.error_message},
                {"terminalCause", "backendError"},
                {"backendAbortState", "backend-error"},
                {"waitingOnBackendOrToolFormatting", false},
                {"syntheticStream", true}
            }));
        }
        write_chunk(error_stream);
        sink.done();
        return false;
    }

    bool tool_repair_attempted = false;
    bool tool_repair_succeeded = false;
    std::string tool_repair_error;
    if (ShouldAttemptToolIntentRepair(result, tool_registry)) {
        tool_repair_attempted = true;
        inferdeck::core::Logger::Get().Warn("Request " + job_id + " attempting tool-intent repair after narrated assistant response");
        auto repair_messages = BuildToolIntentRepairMessages(messages, result, tool_registry);
        auto repair_params = BuildToolIntentRepairParams(params);
        inferdeck::core::InferenceResult repair_result;
        try {
            std::unique_lock<std::mutex> repair_lock(g_switch_mutex);
            repair_result = engine.Predict(repair_messages, repair_params);
        } catch (const std::exception& e) {
            tool_repair_error = e.what();
        } catch (...) {
            tool_repair_error = "Unknown tool-intent repair failure";
        }
        if (tool_repair_error.empty() && !repair_result.HasError() && HasStructuredToolCall(repair_result, tool_registry)) {
            repair_result.prompt_tokens += result.prompt_tokens;
            repair_result.completion_tokens += result.completion_tokens;
            repair_result.total_tokens += result.total_tokens;
            result = std::move(repair_result);
            tool_repair_succeeded = true;
            inferdeck::core::Logger::Get().Info("Request " + job_id + " tool-intent repair succeeded");
        } else if (tool_repair_error.empty()) {
            tool_repair_error = repair_result.HasError()
                ? repair_result.error_message
                : "repair response did not contain a structured tool call";
            inferdeck::core::Logger::Get().Warn("Request " + job_id + " tool-intent repair failed: " + tool_repair_error);
        }
    }

    nlohmann::json response = BuildChatCompletionResponse(id, response_model_id, result, tool_registry);
    const auto synthetic_stream = BuildSyntheticChatCompletionStreamWithRegistry(response, tool_registry);
    bool expected = false;
    if (terminal_sent->compare_exchange_strong(expected, true)) {
        auto preview = SyntheticResultPreview(response,
                                              result,
                                              request_protocol,
                                              "guarded-synthetic-sse",
                                              CountSseDataChunks(synthetic_stream),
                                              heartbeat_chunks_sent,
                                              synthetic_stream.size(),
                                              tool_registry);
        preview["toolRepairAttempted"] = tool_repair_attempted;
        preview["toolRepairSucceeded"] = tool_repair_succeeded;
        if (!tool_repair_error.empty()) preview["toolRepairError"] = tool_repair_error;
        inferdeck::gateway::RuntimeActivity::Get().CompleteJob(
            job_id,
            result,
            preview);
    }
    write_chunk(synthetic_stream);
    sink.done();
    return false;
}
*/

void HandleChatCompletions(const httplib::Request& req, httplib::Response& resp) {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    if (!engine.IsInitialized()) {
        resp.status = 503;
        resp.set_content(R"({"error":{"message":"Engine not initialized","type":"service_error","code":503}})", "application/json");
        return;
    }
    try {
        auto body = json::parse(req.body);
        bool client_requested_stream = body.value("stream", false);
        bool stream = client_requested_stream;

        std::vector<inferdeck::core::ChatMessage> messages;
        if (body.contains("messages")) {
            for (const auto& msg : body["messages"]) {
                inferdeck::core::MessageRole role = inferdeck::core::MessageRole::User;
                if (msg.contains("role")) {
                    auto r = msg["role"].get<std::string>();
                    if (r == "system") role = inferdeck::core::MessageRole::System;
                    else if (r == "assistant") role = inferdeck::core::MessageRole::Assistant;
                    else if (r == "tool") role = inferdeck::core::MessageRole::Tool;
                }
                std::string content;
                std::vector<std::vector<uint8_t>> images;
                auto& c = msg["content"];
                if (c.is_string()) {
                    content = c.get<std::string>();
                } else if (c.is_array()) {
                    for (const auto& part : c) {
                        if (part.contains("type") && part["type"] == "text" && part.contains("text")) {
                            content += part["text"].get<std::string>();
                        } else if (part.contains("type") && part["type"] == "image_url" && part.contains("image_url")) {
                            const auto& iu = part["image_url"];
                            std::string url = iu.is_string() ? iu.get<std::string>() : iu.value("url", "");
                            if (url.find("data:image/") == 0) {
                                auto comma = url.find(",", url.find(";base64,"));
                                if (comma != std::string::npos) {
                                    auto b64 = url.substr(comma + 1);
                                    images.push_back(inferdeck::core::Base64Decode(b64));
                                }
                            }
                        } else if (part.is_string()) {
                            content += part.get<std::string>();
                        }
                    }
                }
                std::string tool_call_id = msg.value("tool_call_id", "");
                std::string name = msg.value("name", "");
                std::string tool_calls_json;
                if (role == inferdeck::core::MessageRole::Assistant && msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                    tool_calls_json = msg["tool_calls"].dump();
                }
                if (role == inferdeck::core::MessageRole::Tool) {
                    content = CompactToolResultContent(content);
                }
                messages.push_back({role, content, tool_call_id, name, tool_calls_json, std::move(images)});
            }
        }

        inferdeck::core::InferenceParams params;
        params.max_tokens = body.value("max_tokens", -1);
        if (body.contains("temperature")) params.temperature = body["temperature"].get<float>();
        if (body.contains("top_p")) params.top_p = body["top_p"].get<float>();
        if (body.contains("stop")) {
            if (body["stop"].is_string()) params.stop = body["stop"].get<std::string>();
        }
        const std::string accepted_client = SafeLogText(DetectChatClientName(req), 80);
        const std::string request_protocol = SafeLogText(req.get_header_value("X-InferDeck-Protocol").empty()
            ? "/v1/chat/completions"
            : req.get_header_value("X-InferDeck-Protocol"), 80);
        const bool request_has_tools = ShouldForceNonStreamingBackend(body);
        bool force_non_streaming_backend = request_has_tools;
        const nlohmann::json request_tools = body.contains("tools") ? body["tools"] : nlohmann::json::array();
        const auto tool_registry = BuildToolNameRegistry(request_tools);
        if (body.contains("tools") && body["tools"].is_array()) {
            params.tools_json = body["tools"].dump();
        }
        std::string tool_format_header = req.get_header_value("X-InferDeck-Tool-Format");
        if (!tool_format_header.empty()) {
            auto hdr = LowerCopy(tool_format_header);
            if (hdr == "qwen" || hdr == "harmony" || hdr == "universal") {
                params.tool_format_override = hdr;
            }
        }
        stream = ShouldUseStreamingBackendForClient(body, accepted_client);

        auto& activity = inferdeck::gateway::RuntimeActivity::Get();
        const std::string accepted_model = body.value("model", "local-model");
        std::string job_id = activity.StartJob(
            request_has_tools ? "tool_chat.completion" : "chat.completion",
            accepted_client,
            accepted_model,
            BuildJobPayloadPreview(body, client_requested_stream, force_non_streaming_backend, request_protocol),
            request_has_tools ? 80 : 50
        );
        resp.set_header("X-InferDeck-Job-Id", job_id);
        const std::string user_agent = SafeLogText(req.get_header_value("User-Agent"));
        const std::string remote_addr = SafeLogText(req.remote_addr, 64);
        inferdeck::core::Logger::Get().Info(
            "Accepted /v1/chat/completions request id=" + job_id +
            " client=" + accepted_client +
            " protocol=" + request_protocol +
            " remote=" + remote_addr +
            " userAgent=" + user_agent +
            " model=" + accepted_model +
            " stream=" + std::string(client_requested_stream ? "true" : "false") +
            " tools=" + std::to_string(body.contains("tools") && body["tools"].is_array() ? body["tools"].size() : 0) +
            " messages=" + std::to_string(body.contains("messages") && body["messages"].is_array() ? body["messages"].size() : 0)
        );
        inferdeck::core::Logger::Get().Info("Request " + job_id + " entering model selection");

        std::unique_lock<std::mutex> model_lock(g_switch_mutex);
        std::string current_clean_id = GetCurrentCleanModelId();
        std::string response_model_id = current_clean_id;

        std::string requested_model = body.value("model", "");
        if (!requested_model.empty()) {
            current_clean_id = GetCurrentCleanModelId();
            response_model_id = current_clean_id;

            std::string normalized_requested = NormalizeId(requested_model);
            std::string normalized_current = NormalizeId(current_clean_id);
            inferdeck::core::Logger::Get().Info("Model request normalized: requested='" + normalized_requested + "' current='" + normalized_current + "'");

            bool needs_switch = (normalized_requested != normalized_current);

            if (needs_switch) {
                std::string model_path = FindModelPath(requested_model);
                if (model_path.empty() && normalized_requested == "gpt-oss-20b") {
                    std::filesystem::path gpt_path = "C:/Users/david/Documents/00_Models/openai_gpt-oss-20b-GGUF/openai_gpt-oss-20b-MXFP4.gguf";
                    if (std::filesystem::exists(gpt_path)) model_path = gpt_path.string();
                }
                if (!model_path.empty()) {
                    inferdeck::core::Logger::Get().Info("Switching model for request '" + requested_model + "' -> " + model_path);
                    if (engine.SwitchModel(model_path)) {
                        response_model_id = MakeCleanModelId(model_path);
                    } else {
                        inferdeck::core::Logger::Get().Error("Failed to switch model for request '" + requested_model + "'");
                    }
                } else {
                    inferdeck::core::Logger::Get().Warn("Requested model not found in model directory: " + requested_model);
                    response_model_id = requested_model;
                }
            }
        }

        if (stream) {
            inferdeck::core::Logger::Get().Info("Request " + job_id + " using backend streaming SSE");
            std::string id = MakeId();
            auto started = std::make_shared<std::atomic<bool>>(false);
            auto lock_holder = std::make_shared<std::unique_lock<std::mutex>>(std::move(model_lock));

            resp.set_chunked_content_provider(
                "text/event-stream",
                [&engine, messages, params, id, response_model_id, job_id, started, lock_holder, request_protocol](size_t, httplib::DataSink& sink) -> bool {
                    bool expected = false;
                    if (!started->compare_exchange_strong(expected, true)) {
                        sink.done();
                        return false;
                    }
                    std::uint64_t sse_chunks_sent = 0;
                    std::uint64_t sse_bytes_sent = 0;
                    std::uint64_t heartbeat_chunks_sent = 0;
                    std::atomic<bool> client_disconnected{false};
                    auto write_chunk = [&](const std::string& chunk) {
                        if (client_disconnected.load()) return false;
                        sse_chunks_sent += CountSseDataChunks(chunk);
                        sse_bytes_sent += chunk.size();
                        const bool ok = sink.write(chunk.data(), chunk.size());
                        if (!ok) client_disconnected.store(true);
                        return ok;
                    };
                    auto write_heartbeat = [&]() {
                        const std::string heartbeat = ": inferdeck backend still running\n\n";
                        if (!write_chunk(heartbeat)) {
                            inferdeck::gateway::RuntimeActivity::Get().FailJob(job_id, "stream client disconnected", 499);
                            inferdeck::core::LlamaEngine::Get().AbortActiveRequest("stream client disconnected");
                            return;
                        }
                        ++heartbeat_chunks_sent;
                    };
                    try {
                        inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, json({
                            {"requestProtocol", request_protocol},
                            {"responseProtocol", "openai.sse"},
                            {"responseMode", "backend-stream"},
                            {"waitingOnBackendOrToolFormatting", true}
                        }), "OpenAI SSE backend stream started");
                        auto role_chunk = SseDeltaChunk(id, response_model_id, json({{"role", "assistant"}}));
                        if (!write_chunk(role_chunk)) {
                            inferdeck::gateway::RuntimeActivity::Get().FailJob(job_id, "stream client disconnected", 499);
                            inferdeck::core::LlamaEngine::Get().AbortActiveRequest("stream client disconnected");
                            sink.done();
                            return false;
                        }
                        std::string streamed_content;
                        std::size_t streamed_content_sent = 0;
                        auto on_token = [&](const std::string& token, inferdeck::core::TokenType type, int) {
                            std::string chunk;
                            if (type == inferdeck::core::TokenType::Content) {
                                streamed_content += token;
                                const auto clean_content = SanitizeAssistantContent(streamed_content);
                                if (clean_content.size() < streamed_content_sent) {
                                    streamed_content_sent = clean_content.size();
                                }
                                if (clean_content.size() > streamed_content_sent) {
                                    chunk = SseChunk(id, response_model_id, clean_content.substr(streamed_content_sent), "", false);
                                    streamed_content_sent = clean_content.size();
                                }
                            } else if (type == inferdeck::core::TokenType::Reasoning) {
                                chunk = SseChunk(id, response_model_id, "", token, false);
                            }
                            if (!chunk.empty() && !write_chunk(chunk)) {
                                inferdeck::gateway::RuntimeActivity::Get().FailJob(job_id, "stream client disconnected", 499);
                                inferdeck::core::LlamaEngine::Get().AbortActiveRequest("stream client disconnected");
                            }
                        };
                        auto result = engine.PredictStream(messages, params, on_token, write_heartbeat);
                        if (lock_holder->owns_lock()) lock_holder->unlock();
                        inferdeck::gateway::RuntimeActivity::Get().CompleteJob(job_id, result, json({
                            {"requestProtocol", request_protocol},
                            {"responseProtocol", "openai.sse"},
                            {"responseMode", "backend-stream"},
                            {"streamed", true},
                            {"sseChunks", sse_chunks_sent},
                            {"heartbeatChunks", heartbeat_chunks_sent},
                            {"responseBytes", sse_bytes_sent},
                            {"finishReason", result.HasError() ? "error" : "stop"},
                            {"toolCallCount", result.tool_calls.size()},
                            {"waitingOnBackendOrToolFormatting", false},
                            {"contentPreview", result.text.substr(0, 240)},
                            {"reasoningPreview", result.reasoning_text.substr(0, 240)}
                        }));
                        if (result.HasError()) write_chunk(SseErrorChunk(id, response_model_id, result.error_message));
                        write_chunk(SseChunk(id, response_model_id, "", "", true));
                        write_chunk("data: [DONE]\n\n");
                    } catch (const std::exception& e) {
                        if (lock_holder->owns_lock()) lock_holder->unlock();
                        inferdeck::gateway::RuntimeActivity::Get().FailJob(job_id, e.what());
                        write_chunk(SseErrorChunk(id, response_model_id, e.what()));
                        write_chunk(SseChunk(id, response_model_id, "", "", true));
                        write_chunk("data: [DONE]\n\n");
                    }
                    if (lock_holder->owns_lock()) lock_holder->unlock();
                    sink.done();
                    return false;
                }
            );
            resp.set_header("Cache-Control", "no-cache");
            resp.set_header("Connection", "keep-alive");
            return;
        }

        if (client_requested_stream && force_non_streaming_backend) {
            const std::string id = MakeId();
            auto started = std::make_shared<std::atomic<bool>>(false);
            const std::string log_label = request_has_tools ? "tool request" : "plain request";
            if (model_lock.owns_lock()) model_lock.unlock();

            resp.set_chunked_content_provider(
                "text/event-stream",
                [&engine, messages, params, id, response_model_id, job_id, started, log_label, request_protocol, tool_registry](size_t, httplib::DataSink& sink) -> bool {
                    bool expected = false;
                    if (!started->compare_exchange_strong(expected, true)) {
                        sink.done();
                        return false;
                    }
                    inferdeck::gateway::RuntimeActivity::Get().MergeJobResult(job_id, json({
                        {"requestProtocol", request_protocol},
                        {"responseProtocol", "openai.sse"},
                        {"responseMode", "non-streaming-sse"},
                        {"waitingOnBackendOrToolFormatting", true}
                    }), "Non-streaming SSE waiting on backend");

                    auto result_ptr = std::make_shared<inferdeck::core::InferenceResult>();
                    auto error_ptr = std::make_shared<std::string>();
                    auto done_flag = std::make_shared<std::atomic<bool>>(false);
                    std::thread([&engine, messages, params, result_ptr, error_ptr, done_flag]() {
                        try {
                            std::unique_lock<std::mutex> backend_lock(g_switch_mutex);
                            *result_ptr = engine.Predict(messages, params);
                        } catch (const std::exception& e) { *error_ptr = e.what(); }
                        catch (...) { *error_ptr = "Unknown inference failure"; }
                        done_flag->store(true);
                    }).detach();

                    while (!done_flag->load()) {
                        static constexpr std::chrono::milliseconds kHeartbeatMs(5000);
                        const std::string heartbeat = ": inferdeck " + log_label + " running\n\n";
                        if (!sink.write(heartbeat.data(), heartbeat.size())) {
                            sink.done();
                            return false;
                        }
                        std::this_thread::sleep_for(kHeartbeatMs);
                    }

                    if (!error_ptr->empty()) {
                        const std::string error_stream = SseErrorChunk(id, response_model_id, *error_ptr) +
                                   SseDeltaChunk(id, response_model_id, nlohmann::json::object(), "error") +
                                   "data: [DONE]\n\n";
                        sink.write(error_stream.data(), error_stream.size());
                        sink.done();
                        return false;
                    }

                    if (result_ptr->HasError()) {
                        const std::string error_stream = SseErrorChunk(id, response_model_id, result_ptr->error_message) +
                                   SseDeltaChunk(id, response_model_id, nlohmann::json::object(), "error") +
                                   "data: [DONE]\n\n";
                        sink.write(error_stream.data(), error_stream.size());
                        sink.done();
                        return false;
                    }

                    nlohmann::json response = BuildChatCompletionResponse(id, response_model_id, *result_ptr, tool_registry);
                    const std::string synthetic_stream = BuildSyntheticChatCompletionStreamWithRegistry(response, tool_registry);
                    sink.write(synthetic_stream.data(), synthetic_stream.size());
                    sink.done();
                    return false;
                }
            );
            resp.set_header("Cache-Control", "no-cache");
            resp.set_header("Connection", "keep-alive");
            return;
        }

        auto result = engine.Predict(messages, params);
        bool tool_repair_attempted = false;
        bool tool_repair_succeeded = false;
        std::string tool_repair_error;
        /*
        if (ShouldAttemptToolIntentRepair(result, tool_registry)) {
            tool_repair_attempted = true;
            inferdeck::core::Logger::Get().Warn("Request " + job_id + " attempting tool-intent repair after narrated assistant response");
            auto repair_messages = BuildToolIntentRepairMessages(messages, result, tool_registry);
            auto repair_params = BuildToolIntentRepairParams(params);
            auto repair_result = engine.Predict(repair_messages, repair_params);
            if (!repair_result.HasError() && HasStructuredToolCall(repair_result, tool_registry)) {
                repair_result.prompt_tokens += result.prompt_tokens;
                repair_result.completion_tokens += result.completion_tokens;
                repair_result.total_tokens += result.total_tokens;
                result = std::move(repair_result);
                tool_repair_succeeded = true;
                inferdeck::core::Logger::Get().Info("Request " + job_id + " tool-intent repair succeeded");
            } else {
                tool_repair_error = repair_result.HasError()
                    ? repair_result.error_message
                    : "repair response did not contain a structured tool call";
                inferdeck::core::Logger::Get().Warn("Request " + job_id + " tool-intent repair failed: " + tool_repair_error);
            }
        }
        */
        inferdeck::core::Logger::Get().Info("Request " + job_id + " backend non-streaming completed");
        model_lock.unlock();
        if (result.HasError()) {
            activity.CompleteJob(job_id, result, json({{"error", result.error_message}}));
            SetInferenceError(resp, result);
            return;
        }

        json response = BuildChatCompletionResponse(MakeId(), response_model_id, result, tool_registry);
        auto extraction = AnalyzeInferenceToolExtraction(result, tool_registry);
        activity.CompleteJob(job_id, result, json({
            {"requestProtocol", request_protocol},
            {"responseProtocol", client_requested_stream ? "openai.sse" : "openai.json"},
            {"responseMode", "json"},
            {"sseChunks", 0},
            {"responseBytes", response.dump().size()},
            {"finishReason", ResponseFinishReason(response)},
            {"toolCallCount", ResponseToolCallCount(response)},
            {"rawToolTextPreview", extraction.raw_preview},
            {"extractedToolCallCount", extraction.calls.size()},
            {"toolExtractionFormat", extraction.format},
            {"toolExtractionError", extraction.error},
            {"toolRepairAttempted", tool_repair_attempted},
            {"toolRepairSucceeded", tool_repair_succeeded},
            {"toolRepairError", tool_repair_error},
            {"waitingOnBackendOrToolFormatting", false},
            {"contentPreview", result.text.substr(0, 500)},
            {"reasoningPreview", result.reasoning_text.substr(0, 500)},
            {"toolCalls", response["choices"][0]["message"].contains("tool_calls") ? response["choices"][0]["message"]["tool_calls"] : json::array()},
            {"usage", response["usage"]}
        }));
        if (client_requested_stream) {
            resp.set_header("Cache-Control", "no-cache");
            resp.set_header("Connection", "keep-alive");
            resp.set_content(BuildSyntheticChatCompletionStreamWithRegistry(response, tool_registry), "text/event-stream");
            return;
        }

        resp.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        resp.status = 400;
        resp.set_content(json({{"error", {{"message", e.what()}, {"type", "invalid_request_error"}, {"code", 400}}}}).dump(), "application/json");
    }
}

void HandleChatCompletionsStream(const httplib::Request& req, httplib::Response& resp) {
    HandleChatCompletions(req, resp);
}

} // namespace
