#include "routes/Health.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleHealth(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json response;
    response["status"] = "ok";
    auto& engine = inferdeck::core::LlamaEngine::Get();
    response["engine_ready"] = engine.IsInitialized();
    response["model"] = engine.GetModelName();
    resp.set_content(response.dump(), "application/json");
}
}
