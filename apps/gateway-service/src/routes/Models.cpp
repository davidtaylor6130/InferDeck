#include "routes/Models.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "config/ConfigLoader.hpp"
#include "core/Config.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <unordered_set>
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

static std::string NormalizeModelName(const std::string& name) {
    std::string lower = name;
    for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));

    static const std::vector<std::string> quant_suffixes = {
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

    std::string clean = name;
    for (const auto& suffix : quant_suffixes) {
        size_t pos = lower.rfind(suffix);
        if (pos != std::string::npos && pos + suffix.size() == lower.size()) {
            clean = name.substr(0, pos);
            break;
        }
    }

    for (auto& c : clean) {
        if (c == ' ' || c == '_' || c == '-') c = '-';
        else c = std::tolower(static_cast<unsigned char>(c));
    }

    return clean;
}

static std::string MakeCleanModelId(const std::string& full_path) {
    if (full_path.empty()) return "local-model";
    std::filesystem::path p(full_path);
    std::string name = p.stem().string();
    return NormalizeModelName(name);
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
    std::vector<std::string> skip_keywords = {"mmproj"};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            std::string filename = entry.path().filename().string();
            std::string lower_filename = filename;
            for (auto& c : lower_filename) c = std::tolower(static_cast<unsigned char>(c));
            bool skip = false;
            for (const auto& keyword : skip_keywords) {
                if (lower_filename.find(keyword) != std::string::npos) {
                    skip = true;
                    break;
                }
            }
            if (skip) continue;
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
        std::string stem = engine.GetModelName();
        model_files.push_back(stem);
        std::string clean_id = NormalizeModelName(stem);
        nlohmann::json model;
        model["id"] = clean_id;
        model["object"] = "model";
        model["created"] = std::time(nullptr);
        model["owned_by"] = "inferdeck";
        model["name"] = stem;
        nlohmann::json aliases = nlohmann::json::array();
        aliases.push_back(clean_id);
        aliases.push_back(clean_id + ":latest");
        response["data"].push_back(model);
        resp.set_content(response.dump(), "application/json");
        return;
    }

    std::unordered_set<std::string> seen_clean_ids;

    for (const auto& model_file : model_files) {
        std::string clean_id = MakeCleanModelId(model_file);
        std::string full_id = MakeModelId(model_file);
        std::string display = MakeDisplayName(model_file);

        if (seen_clean_ids.count(clean_id)) continue;
        seen_clean_ids.insert(clean_id);

        nlohmann::json model;
        model["id"] = clean_id;
        model["object"] = "model";
        model["created"] = std::time(nullptr);
        model["owned_by"] = "inferdeck";
        model["name"] = display;

        nlohmann::json aliases = nlohmann::json::array();
        aliases.push_back(clean_id);
        aliases.push_back(clean_id + ":latest");
        aliases.push_back(full_id);
        aliases.push_back(full_id + ":latest");
        model["aliases"] = aliases;

        response["data"].push_back(model);
    }

    resp.set_content(response.dump(), "application/json");
}
}
