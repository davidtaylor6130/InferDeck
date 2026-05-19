#include "routes/Models.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleModels(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json response;
    response["object"] = "list";
    response["data"] = nlohmann::json::array();
    auto& engine = inferdeck::core::LlamaEngine::Get();
    if (engine.IsInitialized()) {
        nlohmann::json model;
        model["id"] = engine.GetModelName();
        model["object"] = "model";
        model["created"] = std::time(nullptr);
        model["owned_by"] = "inferdeck";
        response["data"].push_back(model);
    }
    resp.set_content(response.dump(), "application/json");
}
}
