/// @file Metrics.cpp
/// @brief /inferdeck/metrics route handler implementation.

#include "routes/Metrics.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "core/Metrics.hpp"
#include "core/Logger.hpp"

#include <nlohmann/json.hpp>

namespace inferdeck::gateway::routes {

void HandleMetrics(const httplib::Request& /*req*/, httplib::Response& resp) {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    auto& metrics = inferdeck::core::MetricsStore::Get();
    auto stats = engine.GetStats();
    auto gpu_info = engine.GetGpuInfo();

    nlohmann::json j;
    j["jobs_submitted"] = stats.total_requests;
    j["jobs_completed"] = stats.successful_requests;
    j["jobs_failed"] = stats.failed_requests;
    j["queue_length"] = 0; // V1: queue length not exposed
    j["average_latency_ms"] = stats.avg_latency_ms;
    j["max_latency_ms"] = stats.max_latency_ms;
    j["min_latency_ms"] = stats.min_latency_ms == 999999.0f ? 0.0f : stats.min_latency_ms;
    j["gpu_memory_used_mb"] = gpu_info.memory_total / (1024 * 1024);
    j["gpu_memory_total_mb"] = gpu_info.memory_total / (1024 * 1024);
    j["tokens_generated"] = stats.tokens_generated;
    j["tokens_processed"] = stats.tokens_processed;

    resp.set_content(j.dump(2), "application/json");
    resp.status = 200;
}

} // namespace inferdeck::gateway::routes
