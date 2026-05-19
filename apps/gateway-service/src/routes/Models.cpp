#include "routes/Models.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <vector>
namespace inferdeck::gateway::routes {

static std::string MakeModelId(const std::string& full_path) {
    if (full_path.empty()) return "local-model";
    std::string name = full_path;
    std::vector<std::string> extensions = {".gguf", ".bin", ".ggml", ".pt", ".pth"};
    for (const auto& ext : extensions) {
        if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            name = name.substr(0, name.size() - ext.size());
            break;
        }
    }
    for (auto& c : name) {
        if (c == ' ' || c == '_' || c == '-') c = '-';
        else c = std::tolower(static_cast<unsigned char>(c));
    }
    return name;
}

void HandleModels(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json response;
    response["object"] = "list";
    response["data"] = nlohmann::json::array();
    auto& engine = inferdeck::core::LlamaEngine::Get();
    if (engine.IsInitialized()) {
        nlohmann::json model;
        model["id"] = MakeModelId(engine.GetModelName());
        model["object"] = "model";
        model["created"] = std::time(nullptr);
        model["owned_by"] = "inferdeck";
        response["data"].push_back(model);
    }
    resp.set_content(response.dump(), "application/json");
}
}
