/// @file Models.cpp
/// @brief /v1/models route handler implementation.

#include "routes/Models.hpp"
#include "llama_cpp/LlamaEngine.hpp"

#include <nlohmann/json.hpp>

namespace inferdeck::gateway::routes {

void HandleModels(const httplib::Request& /*req*/, httplib::Response& resp) {
    auto& engine = inferdeck::core::LlamaEngine::Get();

    nlohmann::json j;
    j["object"] = "list";
    j["data"] = nlohmann::json::array();

    if (engine.IsInitialized()) {
        nlohmann::json model;
        model["id"] = engine.GetModelName();
        model["object"] = "model";
        model["created"] = static_cast<int>(std::time(nullptr));
        model["owned_by"] = "inferdeck";
        j["data"].push_back(model);
    }

    resp.set_content(j.dump(2), "application/json");
    resp.status = 200;
}

} // namespace inferdeck::gateway::routes
