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
    if (id.size() > 8 && id.compare(id.size() - 8, 8, ":latest") == 0) {
        id = id.substr(0, id.size() - 8);
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

json ToOpenAiChatBody(const json& ollama) {
    json body;
    body["model"] = ollama.value("model", "");
    body["messages"] = ollama.value("messages", json::array());
    body["stream"] = ollama.value("stream", false);
    if (ollama.contains("tools")) body["tools"] = ollama["tools"];
    if (ollama.contains("options") && ollama["options"].is_object()) {
        const auto& options = ollama["options"];
        if (options.contains("temperature")) body["temperature"] = options["temperature"];
        if (options.contains("top_p")) body["top_p"] = options["top_p"];
        if (options.contains("num_predict")) body["max_tokens"] = options["num_predict"];
    }
    return body;
}

} // namespace

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
        json body = ToOpenAiChatBody(json::parse(req.body));
        httplib::Request translated = req;
        translated.body = body.dump();
        HandleChatCompletions(translated, resp);
    } catch (const std::exception& e) {
        resp.status = 400;
        resp.set_content(json({{"error", e.what()}}).dump(), "application/json");
    }
}

} // namespace inferdeck::gateway::routes
