#include "routes/Metrics.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include <nlohmann/json.hpp>
namespace inferdeck::gateway::routes {
void HandleMetrics(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json response;
    auto& engine = inferdeck::core::LlamaEngine::Get();
    auto stats = engine.GetStats();
    response["total_requests"] = stats.total_requests;
    response["successful_requests"] = stats.successful_requests;
    response["avg_latency_ms"] = stats.avg_latency_ms;
    response["tokens_generated"] = stats.tokens_generated;
    resp.set_content(response.dump(), "application/json");
}
}
