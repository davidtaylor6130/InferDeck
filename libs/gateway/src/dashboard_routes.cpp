#include "gateway/dashboard_routes.hpp"

#include "foundation/logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace inferdeck::gateway {

using namespace inferdeck::foundation;

namespace {

namespace model = inferdeck::model;
namespace observability = inferdeck::observability;

nlohmann::json gpu_hardware_json(const observability::GpuStats& gpu) {
    double total_mb = 0.0;
    if (!gpu.reason.empty()) {
        const std::string key = "vram_total_mb=";
        auto pos = gpu.reason.find(key);
        if (pos != std::string::npos) {
            try { total_mb = std::stod(gpu.reason.substr(pos + key.size())); } catch (...) { total_mb = 0.0; }
        }
    }
    const double memory_percent = total_mb > 0.0 ? std::clamp((gpu.vram_mb / total_mb) * 100.0, 0.0, 100.0) : 0.0;
    nlohmann::json gpu_json = {
        {"name", gpu.gpu_name.empty() ? "Windows GPU" : gpu.gpu_name},
        {"backend", gpu.provider},
        {"utilization", gpu.utilization_pct},
        {"usage", gpu.utilization_pct},
        {"memoryUsed", gpu.vram_mb * 1024.0 * 1024.0},
        {"memoryPercent", memory_percent},
        {"vramUsed", gpu.vram_mb * 1024.0 * 1024.0},
        {"vramPercent", memory_percent},
        {"temperature", gpu.temperature_c},
        {"power", gpu.power_w}
    };
    if (total_mb > 0.0) {
        gpu_json["memoryTotal"] = total_mb * 1024.0 * 1024.0;
        gpu_json["vramTotal"] = total_mb * 1024.0 * 1024.0;
    } else {
        gpu_json["memoryTotal"] = nullptr;
        gpu_json["vramTotal"] = nullptr;
    }
    return {
        {"available", gpu.available},
        {"provider", gpu.provider},
        {"reason", gpu.reason},
        {"timestamp_unix_ms", gpu.timestamp_unix_ms},
        {"gpu", gpu_json}
    };
}

nlohmann::json system_hardware_json() {
    nlohmann::json out = nlohmann::json::object();
#ifdef _WIN32
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        const auto used = mem.ullTotalPhys - mem.ullAvailPhys;
        out["memory"] = {
            {"used", static_cast<double>(used)},
            {"total", static_cast<double>(mem.ullTotalPhys)},
            {"percentage", static_cast<double>(mem.dwMemoryLoad)}
        };
    }
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    out["cpu"] = {
        {"name", "Windows host CPU"},
        {"logicalProcessors", static_cast<unsigned int>(info.dwNumberOfProcessors)}
    };
#endif
    return out;
}

nlohmann::json build_dashboard_models(model::BackendCoordinator& coordinator) {
    nlohmann::json models = nlohmann::json::array();
    auto loaded = coordinator.get_loaded_model();
    for (const auto& name : coordinator.registry().list()) {
        const auto& info = coordinator.registry().get_info(name);
        models.push_back({
            {"id", name},
            {"name", name},
            {"family", info.family},
            {"loaded", loaded && *loaded == name},
            {"context_size", info.context_size},
            {"vram_required_mb", info.vram_required_mb},
            {"n_slots", info.n_slots},
            {"has_vision", info.has_vision},
        });
    }
    nlohmann::json running = nlohmann::json::array();
    if (loaded) {
        const auto& info = coordinator.registry().get_info(*loaded);
        running.push_back({
            {"id", *loaded},
            {"name", *loaded},
            {"loaded", true},
            {"context_size", info.context_size},
            {"vram_required_mb", info.vram_required_mb},
        });
    }
    return {{"models", models}, {"running", running}, {"current", loaded.value_or("")}};
}

nlohmann::json build_dashboard_jobs(const observability::StatsDb& stats_db, int limit = 100) {
    nlohmann::json jobs = nlohmann::json::array();
    int index = 0;
    for (const auto& row : stats_db.recent_requests(limit)) {
        std::time_t seconds = static_cast<std::time_t>(row.timestamp_unix_ms / 1000);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &seconds);
#else
        gmtime_r(&seconds, &tm);
#endif
        char timestamp[32]{};
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
        jobs.push_back({
            {"id", "request-" + std::to_string(row.timestamp_unix_ms) + "-" + std::to_string(index++)},
            {"type", "chat.completion"},
            {"status", row.status_code >= 200 && row.status_code < 300 ? "succeeded" : "failed"},
            {"model", row.model},
            {"createdAt", timestamp},
            {"timestampUnixMs", row.timestamp_unix_ms},
            {"promptTokens", row.prompt_tokens},
            {"completionTokens", row.completion_tokens},
            {"totalTokens", row.prompt_tokens + row.completion_tokens},
            {"tokensPerSecond", row.tokens_per_second},
            {"durationMs", row.duration_ms},
            {"httpStatus", row.status_code},
            {"slotId", row.slot_id}
        });
    }
    return {{"jobs", jobs}};
}

double percentile(std::vector<double>& sorted_values, double p) {
    if (sorted_values.empty()) return 0.0;
    const double rank = p * (static_cast<double>(sorted_values.size()) - 1.0);
    const auto lo = static_cast<std::size_t>(std::floor(rank));
    const auto hi = static_cast<std::size_t>(std::ceil(rank));
    if (hi >= sorted_values.size()) return sorted_values.back();
    const double frac = rank - static_cast<double>(lo);
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * frac;
}

nlohmann::json build_dashboard_status(const DashboardDeps& deps) {
    auto& coordinator = deps.gw.coordinator;
    const auto& metrics = *deps.gw.metrics;
    const auto& stats_db = *deps.gw.stats_db;

    auto hardware = gpu_hardware_json(deps.gpu.latest());
    auto system = system_hardware_json();
    for (auto it = system.begin(); it != system.end(); ++it) hardware[it.key()] = it.value();

    nlohmann::json usage = nlohmann::json::array();
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t requests = 0;
    for (const auto& row : stats_db.model_usage()) {
        prompt_tokens += row.prompt_tokens;
        completion_tokens += row.completion_tokens;
        requests += row.requests;
        const double avg_tps = row.total_duration_ms > 0.0
            ? static_cast<double>(row.completion_tokens) / (row.total_duration_ms / 1000.0)
            : 0.0;
        usage.push_back({
            {"model", row.model},
            {"requests", row.requests},
            {"successfulRequests", row.successful_requests},
            {"promptTokens", row.prompt_tokens},
            {"completionTokens", row.completion_tokens},
            {"totalTokens", row.prompt_tokens + row.completion_tokens},
            {"peakTokensPerSecond", row.peak_tokens_per_second},
            {"avgTokensPerSecond", avg_tps},
            {"lastTimestampUnixMs", row.last_timestamp_unix_ms}
        });
    }

    auto bucket_json = [](const std::vector<observability::UsageBucketRow>& rows) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& row : rows) {
            out.push_back({
                {"bucket", row.bucket},
                {"model", row.model},
                {"promptTokens", row.prompt_tokens},
                {"completionTokens", row.completion_tokens},
                {"totalTokens", row.total_tokens},
                {"requests", row.requests},
                {"successfulRequests", row.successful_requests}
            });
        }
        return out;
    };
    auto monthly = bucket_json(stats_db.monthly_usage());
    auto daily = bucket_json(stats_db.daily_usage(31));
    auto hourly = bucket_json(stats_db.hourly_usage(24));

    std::vector<double> latencies;
    for (const auto& row : stats_db.recent_requests(500)) {
        if (row.status_code >= 200 && row.status_code < 300 && row.duration_ms > 0.0) {
            latencies.push_back(row.duration_ms);
        }
    }
    std::sort(latencies.begin(), latencies.end());

    const auto swap = deps.gw.swap_tracker ? deps.gw.swap_tracker->snapshot() : SwapSnapshot{};
    auto loaded = coordinator.get_loaded_model();
    auto model_json = build_dashboard_models(coordinator);
    return {
        {"status", "ok"},
        {"queue", {
            {"running", coordinator.active_request_count()},
            {"gpuLocked", coordinator.active_request_count() > 0},
            {"lockOwner", loaded.value_or("")}
        }},
        {"swap", {
            {"swapping", swap.swapping},
            {"target", swap.target},
            {"from", swap.from},
            {"startedUnixMs", swap.started_unix_ms},
            {"lastError", swap.last_error}
        }},
        {"hardware", hardware},
        {"summary", {
            {"totalRequests", requests},
            {"totalTokens", prompt_tokens + completion_tokens},
            {"promptTokens", prompt_tokens},
            {"completionTokens", completion_tokens},
            {"avgLatencyMs", metrics.total_requests() > 0 ? metrics.total_duration_ms() / static_cast<double>(metrics.total_requests()) : 0.0},
            {"p50LatencyMs", percentile(latencies, 0.50)},
            {"p95LatencyMs", percentile(latencies, 0.95)}
        }},
        {"metrics", {
            {"total_requests", metrics.total_requests()},
            {"total_swaps", metrics.total_swaps()},
            {"total_tokens", prompt_tokens + completion_tokens},
            {"avg_tokens_per_second", metrics.avg_tokens_per_second()}
        }},
        {"tokenUsage", usage},
        {"monthlyTokenUsage", monthly},
        {"dailyTokenUsage", daily},
        {"hourlyTokenUsage", hourly},
        {"models", model_json["models"]},
        {"current", model_json["current"]},
        {"uptime", deps.uptime_seconds ? deps.uptime_seconds() : 0}
    };
}

} // namespace

void register_dashboard_routes(httplib::Server& server, const DashboardDeps& deps,
                               const RouteWrapper& wrap) {
    server.Get(R"(^/api/status$)", wrap([deps](const httplib::Request& req,
                                               httplib::Response& resp) {
        (void)req;
        resp.set_content(build_dashboard_status(deps).dump(), "application/json");
    }));

    server.Get(R"(^/api/jobs$)", wrap([deps](const httplib::Request& req,
                                             httplib::Response& resp) {
        int limit = 100;
        if (req.has_param("limit")) {
            try { limit = std::clamp(std::stoi(req.get_param_value("limit")), 1, 500); } catch (...) {}
        }
        resp.set_content(build_dashboard_jobs(*deps.gw.stats_db, limit).dump(), "application/json");
    }));

    server.Post(R"(^/api/models/load$)", wrap([deps](const httplib::Request& req,
                                                     httplib::Response& resp) {
        auto body = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
        const std::string model_name = body.value("model", body.value("name", ""));
        if (model_name.empty()) {
            write_error(resp, 400, "missing_model", "request body must include model");
            return;
        }
        auto started = start_swap_async(deps.gw, model_name);
        write_json(resp, started.status, started.body);
    }));

    server.Post(R"(^/api/models/unload$)", wrap([deps](const httplib::Request& req,
                                                       httplib::Response& resp) {
        (void)req;
        const auto current = deps.gw.coordinator.get_loaded_model();
        auto result = deps.gw.coordinator.unload_current();
        if (!result) {
            write_error(resp, 500, "unload_failed", result.error().message);
            return;
        }
        if (deps.gw.events && current) {
            deps.gw.events->publish("model", nlohmann::json{
                {"state", "unloaded"},
                {"from", *current},
                {"to", ""},
                {"durationMs", 0.0},
                {"error", ""},
            }.dump());
        }
        write_json(resp, 200, {{"ok", true}, {"status", "stopped"}});
    }));

    server.Get(R"(^/api/pricing$)", wrap([deps](const httplib::Request& req,
                                                httplib::Response& resp) {
        (void)req;
        nlohmann::json pricing = nlohmann::json::array();
        std::ifstream file(deps.pricing_file);
        if (file.is_open()) {
            try {
                pricing = nlohmann::json::parse(file);
                if (!pricing.is_array()) pricing = nlohmann::json::array();
            } catch (...) {
                pricing = nlohmann::json::array();
            }
        }
        resp.set_content(pricing.dump(), "application/json");
    }));

    server.Get(R"(^/api/logs$)", wrap([deps](const httplib::Request& req,
                                             httplib::Response& resp) {
        std::size_t limit = 250;
        if (req.has_param("limit")) {
            try { limit = std::clamp<std::size_t>(std::stoul(req.get_param_value("limit")), 1, 1000); } catch (...) {}
        }
        std::ifstream file(deps.log_file.empty() ? "logs/gateway.log" : deps.log_file);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
            if (lines.size() > limit) lines.erase(lines.begin());
        }
        nlohmann::json logs = nlohmann::json::array();
        for (const auto& item : lines) {
            logs.push_back({{"message", item}});
        }
        resp.set_content(nlohmann::json{{"logs", logs}}.dump(), "application/json");
    }));

    server.Get(R"(^/api/events/stream$)", [deps](const httplib::Request& req,
                                                 httplib::Response& resp) {
        (void)req;
        if (!deps.gw.events) {
            resp.status = 503;
            return;
        }
        auto sub = deps.gw.events->subscribe();
        LOG_INFO("sse_subscribe", "subscribers={}", deps.gw.events->subscriber_count());
        resp.set_header("Cache-Control", "no-cache");
        resp.set_header("X-Accel-Buffering", "no");
        resp.set_chunked_content_provider(
            "text/event-stream",
            [sub](std::size_t, httplib::DataSink& sink) -> bool {
                if (sub->closed()) return false;
                auto ev = sub->wait_for(std::chrono::milliseconds{2000});
                if (!sink.is_writable()) return false;
                std::string out;
                if (!ev) {
                    out = ": \n\n";
                } else {
                    out = "event: " + ev->name + "\ndata: " + ev->data + "\n\n";
                }
                return sink.write(out.data(), out.size());
            },
            [sub, deps](bool) {
                sub->close();
                LOG_INFO("sse_unsubscribe", "subscribers={}",
                         deps.gw.events ? deps.gw.events->subscriber_count() : 0);
            });
    });
}

} // namespace inferdeck::gateway
