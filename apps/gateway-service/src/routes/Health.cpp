/// @file Health.cpp
/// @brief /v1/health route handler implementation.

#include "routes/Health.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "core/Logger.hpp"

#include <nlohmann/json.hpp>
#include <chrono>

namespace inferdeck::gateway::routes {

void HandleHealth(const httplib::Request& /*req*/, httplib::Response& resp) {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - std::chrono::steady_clock::now()
    ).count();

    nlohmann::json j;
    j["status"] = "ok";
    j["uptime_seconds"] = uptime;
    j["model_loaded"] = engine.IsInitialized();
    j["gpu_available"] = true;

    resp.set_content(j.dump(2), "application/json");
    resp.status = 200;
}

} // namespace inferdeck::gateway::routes
