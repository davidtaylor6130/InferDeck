#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "config.hpp"
#include "foundation/logging.hpp"
#include "gateway/auth.hpp"
#include "gateway/cors.hpp"
#include "gateway/metrics_builder.hpp"
#include "gateway/routes.hpp"
#include "httplib.h"
#include "model/backend_coordinator.hpp"
#include "model/model_registry.hpp"
#include "observability/gpu_telemetry.hpp"
#include "observability/metrics.hpp"
#include "observability/stats_db.hpp"
#include "scheduler/scheduler.hpp"

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_stop{false};
httplib::Server* g_server = nullptr;

void signal_handler(int sig) {
    g_stop.store(true);
    if (g_server) g_server->stop();
    std::cerr << "\nreceived signal " << sig << ", stopping\n";
}

inferdeck::foundation::LogLevel parse_log_level(const std::string& s) {
    using inferdeck::foundation::LogLevel;
    if (s == "trace") return LogLevel::Trace;
    if (s == "debug") return LogLevel::Debug;
    if (s == "warn") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    if (s == "fatal") return LogLevel::Fatal;
    return LogLevel::Info;
}

void persist_state(const std::string& path, const std::string& model) {
    if (path.empty()) return;
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;
    f << "loaded_model: " << model << "\n";
}

} // namespace

int main(int argc, char** argv) {
    using namespace inferdeck;
    using namespace inferdeck::foundation;
    using namespace inferdeck::gateway;

    fs::path config_path = default_config_path();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-c" || a == "--config") {
            if (i + 1 < argc) {
                config_path = argv[++i];
            }
        } else if (a == "-h" || a == "--help") {
            std::cout << "Usage: " << argv[0] << " [-c config.yml]\n";
            return 0;
        } else if (a == "-v" || a == "--version") {
            std::cout << "inferdeck-gateway 2.0.0\n";
            return 0;
        }
    }

    auto cfg = load_config(config_path);
    foundation::LogConfig lc;
    lc.level = parse_log_level(cfg.log_level);
    if (!cfg.log_file.empty()) lc.log_file = cfg.log_file;
    foundation::Logger::instance().initialize(lc);

    LOG_INFO("startup", "inferdeck-gateway 2.0.0 starting");
    LOG_INFO("config", "loaded from {}", config_path.string());

    model::ModelRegistry registry;
    registry.set_factory([](const model::ModelInfo&) -> std::unique_ptr<model::IModel> {
        return nullptr;
    });
    for (const auto& m : cfg.models) {
        registry.register_model(m);
        LOG_INFO("model_registered", "name={} vram_mb={} n_slots={}",
                 m.name, m.vram_required_mb, m.n_slots);
    }
    LOG_WARN("factory_stub", "IModel factory returns nullptr; set_factory must be replaced with real LlamaCppModel in P10");

    model::BackendCoordinator coordinator(registry);
    scheduler::Scheduler scheduler(coordinator);

    observability::Metrics metrics;
    observability::GpuTelemetry gpu;
    observability::StatsDb stats_db(cfg.stats_db_path);
    const auto started_at = std::chrono::steady_clock::now();

    if (!cfg.adlx_helper_path.empty()) {
        gpu.set_helper_path(cfg.adlx_helper_path);
        gpu.set_poll_interval(std::chrono::milliseconds(cfg.telemetry_poll_ms));
        gpu.start();
        LOG_INFO("gpu_telemetry_started", "helper={} poll_ms={}",
                 cfg.adlx_helper_path, cfg.telemetry_poll_ms);
    } else {
        LOG_WARN("gpu_telemetry_disabled", "no adlx_helper configured");
    }

    GatewayDeps deps{coordinator, scheduler, "15"};

    auto uptime_seconds = [&] {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - started_at).count();
    };

    AuthConfig ac;
    ac.required = cfg.auth_required;
    ac.token = cfg.auth_token;
    AuthMiddleware auth(ac);
    CorsMiddleware cors(cfg.cors_origins);

    if (!cfg.default_model.empty() && registry.has(cfg.default_model)) {
        LOG_INFO("default_model_deferred", "name={} (deferred until P10 LlamaCppModel lands)",
                 cfg.default_model);
    }

    httplib::Server server;
    g_server = &server;

    auto wrap = [&](auto handler) {
        return [&, handler](const httplib::Request& req,
                            httplib::Response& resp) {
            cors.apply(resp);
            if (auth.required() && !auth.check(header_value(req, "Authorization"))) {
                resp.status = 401;
                resp.set_header("WWW-Authenticate", "Bearer");
                write_json(resp, 401, {{"error", {{"code", "unauthorized"},
                                                  {"message", "valid Bearer token required"}}}});
                return;
            }
            handler(req, resp);
        };
    };

    server.Get("/v1/models", wrap([&](const httplib::Request& req,
                                      httplib::Response& resp) {
        handle_models(req, resp, deps);
    }));
    server.Post("/v1/swap/to/:name", wrap([&](const httplib::Request& req,
                                              httplib::Response& resp) {
        handle_swap_to(req, resp, deps, req.path_params.at("name"));
    }));
    server.Get("/v1/swap/status", wrap([&](const httplib::Request& req,
                                           httplib::Response& resp) {
        handle_swap_status(req, resp, deps);
    }));
    server.Post("/v1/chat/completions", wrap([&](const httplib::Request& req,
                                                 httplib::Response& resp) {
        handle_chat_completions(req, resp, deps);
    }));
    server.Get("/v1/metrics", wrap([&](const httplib::Request& req,
                                       httplib::Response& resp) {
        resp.set_content(
            MetricsBuilder::build_live(metrics, gpu, uptime_seconds()).dump(),
            "application/json");
    }));
    server.Get("/v1/stats/history", wrap([&](const httplib::Request& req,
                                             httplib::Response& resp) {
        resp.set_content(MetricsBuilder::build_history(stats_db, 100).dump(),
                         "application/json");
    }));
    server.Get("/v1/health", wrap([&](const httplib::Request& req,
                                      httplib::Response& resp) {
        resp.set_content(MetricsBuilder::build_health(metrics, gpu, stats_db).dump(),
                         "application/json");
    }));
    if (cors.handles_options()) {
        server.Options(".*", [&](const httplib::Request& req,
                                  httplib::Response& resp) {
            cors.apply(resp);
            resp.status = 204;
        });
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    LOG_INFO("server_listening", "host={} port={}", cfg.host, cfg.port);
    if (!server.listen(cfg.host.c_str(), cfg.port)) {
        LOG_ERROR("server_failed", "could not bind {}:{}", cfg.host, cfg.port);
        return 1;
    }

    if (!cfg.state_file.empty()) {
        if (auto m = coordinator.get_loaded_model()) {
            persist_state(cfg.state_file, *m);
        }
    }
    LOG_INFO("server_stopped", "graceful shutdown");
    return 0;
}
