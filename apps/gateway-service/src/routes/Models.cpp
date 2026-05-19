#include "routes/Models.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "config/ConfigLoader.hpp"
#include "core/Config.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <vector>
#include <algorithm>
namespace inferdeck::gateway::routes {

static std::string MakeModelId(const std::string& full_path) {
    if (full_path.empty()) return "local-model";
    std::filesystem::path p(full_path);
    std::string name = p.stem().string();
    for (auto& c : name) {
        if (c == ' ' || c == '_' || c == '-') c = '-';
        else c = std::tolower(static_cast<unsigned char>(c));
    }
    return name;
}

static std::string MakeDisplayName(const std::string& full_path) {
    if (full_path.empty()) return "Local Model";
    std::filesystem::path p(full_path);
    return p.stem().string();
}

static std::vector<std::string> ScanModelDirectory(const std::string& dir_path) {
    std::vector<std::string> models;
    if (dir_path.empty() || !std::filesystem::exists(dir_path)) {
        return models;
    }
    std::vector<std::string> extensions = {".gguf", ".bin", ".ggml"};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            std::string filename = entry.path().filename().string();
            if (filename.find("mmproj") == 0) continue;
            for (const auto& e : extensions) {
                if (ext == e) {
                    models.push_back(entry.path().string());
                    break;
                }
            }
        }
    }
    std::sort(models.begin(), models.end());
    return models;
}

void HandleModels(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json response;
    response["object"] = "list";
    response["data"] = nlohmann::json::array();

    auto& engine = inferdeck::core::LlamaEngine::Get();

    std::string model_dir;
    try {
        auto full = inferdeck::core::Config::Load(GetDefaultConfigPath());
        model_dir = full.model.directory;
    } catch (...) {}

    std::vector<std::string> model_files;
    if (!model_dir.empty()) {
        model_files = ScanModelDirectory(model_dir);
    }

    if (model_files.empty() && engine.IsInitialized()) {
        model_files.push_back(engine.GetModelName());
    }

    for (const auto& model_file : model_files) {
        nlohmann::json model;
        model["id"] = MakeModelId(model_file);
        model["object"] = "model";
        model["created"] = std::time(nullptr);
        model["owned_by"] = "inferdeck";
        model["name"] = MakeDisplayName(model_file);
        response["data"].push_back(model);
    }

    resp.set_content(response.dump(), "application/json");
}
}
