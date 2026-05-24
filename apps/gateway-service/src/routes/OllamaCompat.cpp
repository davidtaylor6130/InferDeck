#include "routes/OllamaCompat.hpp"
#include "routes/ChatCompletions.hpp"
#include "config/ConfigLoader.hpp"
#include "core/Config.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <algorithm>
#include <ctime>
#include <cstdint>
#include <vector>

using json = nlohmann::json;

namespace inferdeck::gateway::routes {
namespace {

std::string NormalizeId(std::string id) {
    for (auto& c : id) {
        if (c == ' ' || c == '_') c = '-';
        else c = std::tolower(static_cast<unsigned char>(c));
    }
    constexpr size_t latest_suffix_len = 7;
    if (id.size() > latest_suffix_len && id.compare(id.size() - latest_suffix_len, latest_suffix_len, ":latest") == 0) {
        id = id.substr(0, id.size() - latest_suffix_len);
    }
    return id;
}

std::vector<std::filesystem::path> ScanModels() {
    std::vector<std::filesystem::path> models;
    auto full = inferdeck::core::Config::Load(GetDefaultConfigPath());
    if (full.model.directory.empty() || !std::filesystem::exists(full.model.directory)) {
        return models;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(full.model.directory)) {
        if (!entry.is_regular_file()) continue;
        auto filename = entry.path().filename().string();
        auto lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (entry.path().extension() == ".gguf" && lower.find("mmproj") == std::string::npos) {
            models.push_back(entry.path());
        }
    }
    std::sort(models.begin(), models.end());
    return models;
}

constexpr int kDefaultOllamaNumPredict = 4096;

} // namespace

json BuildOpenAiChatBodyFromOllama(const json& ollama) {
    json body;
    body["model"] = ollama.value("model", "");
    body["messages"] = ollama.value("messages", json::array());
    body["stream"] = ollama.value("stream", false);
    if (ollama.contains("tools")) body["tools"] = ollama["tools"];
    bool has_num_predict = false;
    if (ollama.contains("options") && ollama["options"].is_object()) {
        const auto& options = ollama["options"];
        if (options.contains("temperature")) body["temperature"] = options["temperature"];
        if (options.contains("top_p")) body["top_p"] = options["top_p"];
        if (options.contains("num_predict")) {
            body["max_tokens"] = options["num_predict"];
            has_num_predict = true;
        }
    }
    if (!has_num_predict) {
        body["max_tokens"] = kDefaultOllamaNumPredict;
    }
    return body;
}

std::string BuildOllamaChatStreamFromOpenAiSse(const std::string& openai_sse, const std::string& model) {
    std::string output;
    std::string finish_reason = "stop";

    size_t pos = 0;
    while (pos < openai_sse.size()) {
        size_t line_end = openai_sse.find('\n', pos);
        std::string line = openai_sse.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos);
        pos = line_end == std::string::npos ? openai_sse.size() : line_end + 1;

        if (line.rfind("data: ", 0) != 0) continue;
        std::string payload = line.substr(6);
        if (payload == "[DONE]") break;
        if (payload.empty()) continue;

        try {
            auto chunk = json::parse(payload);
            if (!chunk.contains("choices") || !chunk["choices"].is_array() || chunk["choices"].empty()) continue;
            const auto& choice = chunk["choices"][0];
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                finish_reason = choice["finish_reason"].get<std::string>();
                continue;
            }
            if (!choice.contains("delta") || !choice["delta"].is_object()) continue;
            const auto& delta = choice["delta"];

            json message = {{"role", "assistant"}};
            bool has_payload = false;
            if (delta.contains("content") && delta["content"].is_string() && !delta["content"].get<std::string>().empty()) {
                message["content"] = delta["content"];
                has_payload = true;
            }
            if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string() && !delta["reasoning_content"].get<std::string>().empty()) {
                message["thinking"] = delta["reasoning_content"];
                has_payload = true;
            }
            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                message["tool_calls"] = delta["tool_calls"];
                has_payload = true;
            }
            if (!has_payload) continue;

            output += json({
                {"model", model},
                {"created_at", "2026-05-22T00:00:00Z"},
                {"message", message},
                {"done", false}
            }).dump();
            output += "\n";
        } catch (...) {
            continue;
        }
    }

    output += json({
        {"model", model},
        {"created_at", "2026-05-22T00:00:00Z"},
        {"message", {{"role", "assistant"}, {"content", ""}}},
        {"done_reason", finish_reason},
        {"done", true}
    }).dump();
    output += "\n";
    return output;
}

void HandleOllamaVersion(const httplib::Request&, httplib::Response& resp) {
    resp.status = 200;
    resp.set_content(json({{"version", "inferdeck-llama.cpp-b9276-vulkan"}}).dump(), "application/json");
}

void HandleOllamaTags(const httplib::Request&, httplib::Response& resp) {
    resp.status = 200;
    json response;
    response["models"] = json::array();
    for (const auto& model_path : ScanModels()) {
        std::string id = NormalizeId(model_path.stem().string());
        response["models"].push_back({
            {"name", id + ":latest"},
            {"model", id + ":latest"},
            {"modified_at", "2026-05-22T00:00:00Z"},
            {"size", static_cast<std::uint64_t>(std::filesystem::file_size(model_path))},
            {"digest", id},
            {"details", {{"format", "gguf"}, {"family", id}, {"families", json::array({id})}}}
        });
    }
    resp.set_content(response.dump(), "application/json");
}

void HandleOllamaChat(const httplib::Request& req, httplib::Response& resp) {
    try {
        json ollama_body = json::parse(req.body);
        bool stream = ollama_body.value("stream", false);
        json body = BuildOpenAiChatBodyFromOllama(ollama_body);
        httplib::Request translated = req;
        translated.body = body.dump();
        translated.headers.emplace("X-InferDeck-Client", "Open WebUI");
        HandleChatCompletions(translated, resp);
        if (stream && resp.status < 400 && resp.get_header_value("Content-Type").find("text/event-stream") != std::string::npos) {
            auto ollama_stream = BuildOllamaChatStreamFromOpenAiSse(resp.body, ollama_body.value("model", body.value("model", "")));
            resp.set_content(ollama_stream, "application/x-ndjson");
            return;
        }
        if (!stream && resp.status < 400 && resp.get_header_value("Content-Type").find("application/json") != std::string::npos) {
            auto openai = json::parse(resp.body);
            json message = {{"role", "assistant"}, {"content", ""}};
            std::string finish_reason = "stop";
            if (openai.contains("choices") && openai["choices"].is_array() && !openai["choices"].empty()) {
                const auto& choice = openai["choices"][0];
                finish_reason = choice.value("finish_reason", "stop");
                if (choice.contains("message") && choice["message"].is_object()) {
                    const auto& openai_message = choice["message"];
                    message["role"] = openai_message.value("role", "assistant");
                    message["content"] = openai_message.value("content", "");
                    if (openai_message.contains("reasoning_content") && openai_message["reasoning_content"].is_string()) {
                        message["thinking"] = openai_message["reasoning_content"];
                    }
                    if (openai_message.contains("tool_calls")) {
                        message["tool_calls"] = openai_message["tool_calls"];
                    }
                }
            }
            json ollama_response = {
                {"model", ollama_body.value("model", body.value("model", ""))},
                {"created_at", "2026-05-22T00:00:00Z"},
                {"message", message},
                {"done_reason", finish_reason},
                {"done", true}
            };
            if (openai.contains("usage")) {
                ollama_response["prompt_eval_count"] = openai["usage"].value("prompt_tokens", 0);
                ollama_response["eval_count"] = openai["usage"].value("completion_tokens", 0);
            }
            resp.set_content(ollama_response.dump(), "application/json");
        }
    } catch (const std::exception& e) {
        resp.status = 400;
        resp.set_content(json({{"error", e.what()}}).dump(), "application/json");
    }
}

} // namespace inferdeck::gateway::routes
