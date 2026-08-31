#include <atomic>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#endif

#include "build_info.hpp"
#include "config.hpp"
#include "foundation/event_bus.hpp"
#include "foundation/json_utils.hpp"
#include "foundation/logging.hpp"
#include "foundation/path_utils.hpp"
#include "gateway/auth.hpp"
#include "gateway/cors.hpp"
#include "gateway/deadline_server.hpp"
#include "gateway/dashboard_routes.hpp"
#include "gateway/config_repository.hpp"
#include "gateway/metrics_builder.hpp"
#include "gateway/media_routes.hpp"
#include "gateway/model_store.hpp"
#include "gateway/openai_routes.hpp"
#include "gateway/request_id.hpp"
#include "gateway/request_security.hpp"
#include "gateway/route_manifest.hpp"
#include "gateway/routes.hpp"
#include "gateway/shutdown_state.hpp"
#include "gateway/swap_tracker.hpp"
#include "httplib.h"
#include "llama.h"
#include "llama_cpp_wrapper/llama_cpp_model.hpp"
#include "model/backend_coordinator.hpp"
#include "model/model_registry.hpp"
#include "native_runtimes/runtime_factories.hpp"
#include "observability/gpu_telemetry.hpp"
#include "observability/metrics.hpp"
#include "observability/stats_db.hpp"

namespace fs = std::filesystem;

namespace {

namespace foundation = inferdeck::foundation;
namespace model = inferdeck::model;
namespace observability = inferdeck::observability;

std::atomic<bool> g_stop{false};
std::atomic<bool> g_reload{false};
std::atomic<bool> g_default_model_loading{false};
httplib::Server* g_server = nullptr;
std::once_flag g_llama_init_once;
constexpr int runtime_reload_result = 75;

#include "process_runtime.ipp"
#include "dashboard_static.ipp"
#include "profile_benchmark_runtime.ipp"
int run_gateway(const fs::path& config_path) {
    using namespace inferdeck;
    using namespace inferdeck::foundation;
    using namespace inferdeck::gateway;

    g_stop.store(false);
    g_reload.store(false);
    g_default_model_loading.store(false);
    const auto config_selection = load_config_with_active(config_path);
    auto cfg = config_selection.config;
    const auto running_config_revision =
        config_revision(read_text_file(config_selection.loaded_path.string()));
    foundation::LogConfig lc;
    lc.level = parse_log_level(cfg.log_level);
    if (!cfg.log_file.empty()) lc.log_file = cfg.log_file;
    foundation::Logger::instance().initialize(lc);

    LOG_INFO("startup", "inferdeck-gateway {} revision={} dirty={} starting",
             inferdeck::app::build_version, inferdeck::app::build_revision,
             inferdeck::app::build_dirty);
    LOG_INFO("config", "loaded from {}", config_selection.loaded_path.string());
    if (!config_selection.fallback_reason.empty()) {
        LOG_ERROR("active_config_fallback",
                  "active profile {} was rejected: {}; using stable base {}",
                  config_selection.active_path.string(),
                  config_selection.fallback_reason,
                  config_path.string());
    }
    std::call_once(g_llama_init_once, [] {
        LOG_INFO("vulkan_test", "About to initialize llama backend");
        llama_backend_init();
        const char* sys_info = llama_print_system_info();
        if (sys_info) {
            LOG_INFO("llama_system_info", "{}", sys_info);
        }
    });

    model::ModelRegistry registry;
    registry.register_factory("llama_cpp", [cfg](const model::ModelInfo& info) -> std::unique_ptr<model::IBackend> {
        return std::make_unique<llama_wrapper::LlamaCppModel>(
            info, make_llama_config(cfg, info));
    });
    native_runtimes::register_factories(registry);
    for (const auto& m : cfg.models) {
        registry.register_model(m);
        LOG_INFO("model_registered", "name={} vram_mb={} n_slots={}",
                 m.name, m.vram_required_mb, m.n_slots);
    }
    for (const auto& alias : cfg.model_aliases) {
        const auto registered = registry.set_alias(alias);
        if (!registered) {
            throw std::runtime_error("invalid model alias " + alias.name + ": " +
                                     registered.error().message);
        }
        LOG_INFO("model_alias_registered", "alias={} target={}", alias.name, alias.target);
    }
    LOG_INFO("factory_set", "LlamaCppModel factory installed");

    model::BackendCoordinator coordinator(registry);
    coordinator.set_max_queue_size(static_cast<std::size_t>(std::max(1, cfg.max_queue_size)));
    if (cfg.vram_budget_mb > 0) {
        coordinator.set_vram_budget(cfg.vram_budget_mb, cfg.vram_safety_margin_mb);
    }
    ModelStore model_store(cfg.model_store_root, cfg.model_store_archive_root,
                           cfg.model_store_hf_token, coordinator);

    observability::Metrics metrics;
    observability::GpuTelemetry gpu;
    observability::StatsDb stats_db(cfg.stats_db_path);
    if (stats_db.healthy()) {
        const auto totals = stats_db.lifetime_totals();
        metrics.restore_lifetime(totals.requests, totals.swaps,
                                 totals.prompt_tokens, totals.completion_tokens,
                                 totals.total_duration_ms);
        LOG_INFO("stats_restored",
                 "db={} requests={} swaps={} prompt_tokens={} completion_tokens={} duration_ms={}",
                 stats_db.path(), totals.requests, totals.swaps,
                 totals.prompt_tokens, totals.completion_tokens,
                 totals.total_duration_ms);
    }
    const auto started_at = std::chrono::steady_clock::now();

    gpu.set_helper_path(cfg.adlx_helper_path);
    gpu.set_poll_interval(std::chrono::milliseconds(cfg.telemetry_poll_ms));
    gpu.start();
    LOG_INFO("gpu_telemetry_started", "provider=windows_pdh_dxgi poll_ms={}",
             cfg.telemetry_poll_ms);

    foundation::EventBus events;
    SwapTracker swap_tracker;
    std::atomic<ComputeResource> maintenance_resource{ComputeResource::None};
    GatewayDeps deps{coordinator, "15", cfg.auto_swap,
                     cfg.default_model,
                     cfg.voice_session_grace_ms,
                     &metrics, &stats_db, &events, &swap_tracker,
                     &maintenance_resource};
    auto derivative_deps = deps;
    derivative_deps.compatibility_profile =
        CompatibilityProfile::OpenAIDerivative;
    ProfileBenchmarkManager profile_benchmark{
        coordinator,
        &swap_tracker,
        maintenance_resource,
        [cfg, &gpu](
            const model::ModelInfo& info,
            const optimize::ProfileCandidate& candidate,
            const std::vector<ProfileBenchmarkPrompt>& prompts,
            const std::atomic<bool>& cancel,
            const ProfileBenchmarkProgress& progress) {
            return run_profile_benchmark_trial(
                cfg, gpu, info, candidate, prompts, cancel, progress);
        }};
    ProfileBenchmarkScheduler profile_benchmark_scheduler{
        profile_benchmark, coordinator, gpu};

    auto uptime_seconds = [&] {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - started_at).count();
    };

    std::atomic<bool> stats_stop{false};
    std::thread stats_thread([&] {
        while (!stats_stop.load()) {
            const auto g = gpu.latest();
            if (cfg.vram_budget_mb <= 0 && g.vram_total_mb > 0.0) {
                coordinator.set_vram_budget(static_cast<int>(g.vram_total_mb),
                                            cfg.vram_safety_margin_mb);
            }
            if (events.subscriber_count() > 0) {
                const auto swap = swap_tracker.snapshot();
                events.publish("stats", nlohmann::json{
                    {"timestampUnixMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()},
                    {"gpu", {
                        {"available", g.available},
                        {"name", g.gpu_name},
                        {"utilizationPct", g.utilization_pct},
                        {"vramUsedMb", g.vram_mb},
                        {"vramTotalMb", g.vram_total_mb},
                        {"temperatureC", g.temperature_c},
                        {"powerW", g.power_w},
                    }},
                    {"loadedModel", coordinator.get_loaded_model().value_or("")},
                    {"activeRequests", coordinator.active_request_count()},
                    {"queuedRequests", coordinator.queued_request_count()},
                    {"resourceDecision", coordinator.last_resource_decision()},
                    {"swapping", swap.swapping},
                    {"swapTarget", swap.target},
                    {"totalRequests", metrics.total_requests()},
                    {"totalSwaps", metrics.total_swaps()},
                    {"lifetimeTokensIn", metrics.lifetime_tokens_in()},
                    {"lifetimeTokensOut", metrics.lifetime_tokens_out()},
                    {"avgTokensPerSecond", metrics.avg_tokens_per_second()},
                    {"uptimeSeconds", uptime_seconds()},
                }.dump());
            }
            for (int i = 0; i < 10 && !stats_stop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }
    });

    RouteAuthConfig route_auth;
    route_auth.data_plane.required = cfg.auth_required;
    route_auth.data_plane.token = cfg.auth_token;
    route_auth.control_allow_remote = cfg.control_allow_remote;
    route_auth.control_allow_data_plane_token = cfg.control_allow_data_plane_token;
    route_auth.control_token = cfg.control_token;
    RouteAuthorizer authorizer(std::move(route_auth));
    AuthMiddleware control_session({true, cfg.control_token});
    CorsMiddleware data_cors(cfg.cors_origins);
    CorsMiddleware control_cors(cfg.control_origins);
    const std::string dashboard_session_path =
        std::string(inferdeck::gateway::kControlApiBase) + "/dashboard/session";

    DeadlineServer server(server_request_read_deadline);
    server.set_read_timeout(server_read_timeout);
    server.set_write_timeout(server_write_timeout);
    server.set_keep_alive_timeout(server_keep_alive_timeout.count());
    server.set_payload_max_length(server_body_limit);
    server.new_task_queue = [] {
        return new httplib::ThreadPool(std::max(32u, std::thread::hardware_concurrency() * 2u));
    };
    g_server = &server;
    const auto dashboard_static_dir = find_dashboard_static_dir();
    const auto apply_cors = [&](RoutePrincipal principal,
                                const httplib::Request& req,
                                httplib::Response& resp) {
        if (principal == RoutePrincipal::DashboardSession ||
            principal == RoutePrincipal::ControlRead ||
            principal == RoutePrincipal::ControlWrite) {
            control_cors.apply(req, resp);
            return;
        }
        data_cors.apply(req, resp);
    };
    const auto proxy_indicated = [](const httplib::Request& req) {
        for (const char* header : {"Forwarded", "X-Forwarded-For", "X-Real-IP",
                                   "X-Forwarded-Host", "Via"}) {
            if (req.has_header(header)) return true;
        }
        return false;
    };
    const auto write_validation_error = [](httplib::Response& resp,
                                           RequestValidationStatus validation) {
        if (validation == RequestValidationStatus::BodyNotAllowed) {
            write_error(resp, 400, "body_not_allowed",
                        "request body is not allowed for this endpoint");
        } else if (validation == RequestValidationStatus::PayloadTooLarge) {
            write_error(resp, 413, "request_too_large",
                        "request body exceeds the endpoint limit");
        } else if (validation == RequestValidationStatus::UnsupportedMediaType) {
            write_error(resp, 415, "unsupported_media_type",
                        "request Content-Type is not supported by this endpoint");
        } else if (validation == RequestValidationStatus::InvalidContentLength) {
            write_error(resp, 400, "invalid_content_length",
                        "request Content-Length is invalid");
        } else if (validation == RequestValidationStatus::UnsupportedTransferEncoding) {
            write_error(resp, 411, "content_length_required",
                        "chunked request bodies are not supported");
        }
    };

    server.set_expect_100_continue_handler(
        [&](const httplib::Request& req, httplib::Response& resp) {
            const auto id = request_id(header_value(req, "X-Request-Id"));
            resp.set_header("X-Request-Id", id);
            LOG_INFO("http_request_begin", "request_id={} method={} path={}",
                     id, req.method, req.path);
            apply_cors(classify_route(req.method, req.path), req, resp);
            write_error(resp, 417, "expectation_failed",
                        "Expect: 100-continue is not supported");
            return 400;
        });

    server.set_pre_routing_handler([&](const httplib::Request& req,
                                       httplib::Response& resp) {
        const auto id = request_id(header_value(req, "X-Request-Id"));
        resp.set_header("X-Request-Id", id);
        LOG_INFO("http_request_begin", "request_id={} method={} path={}",
                 id, req.method, req.path);
        const auto principal = classify_route(req.method, req.path);
        apply_cors(principal, req, resp);
        if (req.has_header("Expect")) {
            write_error(resp, 417, "expectation_failed",
                        "Expect request headers are not supported");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (g_stop.load() || g_reload.load()) {
            resp.set_header("Retry-After", "1");
            write_error(resp, 503, "server_stopping",
                        "gateway shutdown or reload is in progress");
            return httplib::Server::HandlerResponse::Handled;
        }
        const bool disabled_derivative =
            req.path.starts_with(kOpenAIDerivativeBase) &&
            !cfg.openai_derivative_compatibility_enabled;
        if (disabled_derivative) {
            write_error(resp, 404, "not_found",
                        "compatibility profile is disabled");
            return httplib::Server::HandlerResponse::Handled;
        }
        const bool proxied = proxy_indicated(req);
        const bool direct_loopback = RouteAuthorizer::is_direct_loopback(
            req.remote_addr, header_value(req, "Host"), proxied);
        const bool control_route = principal == RoutePrincipal::DashboardSession ||
            principal == RoutePrincipal::ControlRead ||
            principal == RoutePrincipal::ControlWrite;
        if (req.method == "OPTIONS") {
            if (control_route) {
                if ((principal == RoutePrincipal::DashboardSession && !direct_loopback) ||
                    (!direct_loopback && !cfg.control_allow_remote)) {
                    write_error(resp, 403, "forbidden",
                                "control-plane access is not available from this client");
                    return httplib::Server::HandlerResponse::Handled;
                }
                const auto origin = header_value(req, "Origin");
                if (origin.empty() || !control_cors.allows_origin(origin)) {
                    write_error(resp, 403, "origin_forbidden",
                                "request origin is not allowed for the control plane");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        }
        const bool dashboard_login =
            req.method == "POST" &&
            req.path == dashboard_session_path;
        auto credential = header_value(req, "Authorization");
        if (credential.empty()) {
            const auto token = cookie_value(
                header_value(req, "Cookie"), "inferdeck_control");
            if (!token.empty()) credential = "Bearer " + token;
        }
        const auto authorization = dashboard_login
            ? AuthorizationStatus::Granted
            : authorizer.authorize(principal, credential, req.remote_addr,
                                   header_value(req, "Host"), proxied);
        if (authorization == AuthorizationStatus::AuthenticationRequired) {
            resp.set_header("WWW-Authenticate", "Bearer");
            write_error(resp, 401, "unauthorized", "valid Bearer token required");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (authorization == AuthorizationStatus::Forbidden) {
            write_error(resp, 403, "forbidden",
                        "control-plane access is not available from this client");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (principal == RoutePrincipal::ControlWrite) {
            const auto origin = header_value(req, "Origin");
            if ((!origin.empty() && !control_cors.allows_origin(origin)) ||
                header_value(req, "Sec-Fetch-Site") == "cross-site") {
                write_error(resp, 403, "origin_forbidden",
                            "request origin is not allowed for the control plane");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        const auto policy = request_policy(
            req.method, req.path, principal == RoutePrincipal::ControlWrite);
        const auto validation = validate_request_headers(req, policy);
        if (validation != RequestValidationStatus::Allowed) {
            write_validation_error(resp, validation);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.set_logger([](const httplib::Request& req,
                         const httplib::Response& resp) {
        auto id = resp.get_header_value("X-Request-Id");
        if (id.empty()) id = request_id(header_value(req, "X-Request-Id"));
        const int status = resp.status > 0 ? resp.status : 200;
        LOG_INFO("http_response_committed", "request_id={} method={} path={} status={}",
                 id, req.method, req.path, status);
    });

    RouteWrapper wrap = [&](httplib::Server::Handler handler) -> httplib::Server::Handler {
        return [&, handler](const httplib::Request& req,
                            httplib::Response& resp) {
            const auto principal = classify_route(req.method, req.path);
            const auto validation = validate_request(
                req, request_policy(req.method, req.path,
                                    principal == RoutePrincipal::ControlWrite));
            if (validation != RequestValidationStatus::Allowed) {
                write_validation_error(resp, validation);
                return;
            }
            try {
                handler(req, resp);
            } catch (const std::exception& e) {
                const auto id = resp.get_header_value("X-Request-Id");
                LOG_ERROR("handler_exception", "request_id={} what={}", id, e.what());
                if (resp.status == 0) resp.status = 500;
                write_error(resp, resp.status, "internal_error", "request could not be completed");
            } catch (...) {
                const auto id = resp.get_header_value("X-Request-Id");
                LOG_ERROR("handler_unknown_exception", "request_id={}", id);
                if (resp.status == 0) resp.status = 500;
                write_error(resp, resp.status, "internal_error", "unknown exception");
            }
        };
    };

    server.Post(dashboard_session_path,
                wrap([&](const httplib::Request& req,
                         httplib::Response& resp) {
        if (!cfg.control_allow_remote) {
            write_error(resp, 403, "forbidden",
                        "remote dashboard access is disabled");
            return;
        }
        const auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() ||
            !body.contains("token") || !body["token"].is_string()) {
            write_error(resp, 400, "invalid_request",
                        "dashboard token is required");
            return;
        }
        const auto token = body["token"].get<std::string>();
        if (!control_session.check("Bearer " + token)) {
            resp.set_header("WWW-Authenticate", "Bearer");
            write_error(resp, 401, "unauthorized",
                        "valid dashboard token required");
            return;
        }
        resp.set_header("Set-Cookie",
                        "inferdeck_control=" + token +
                        "; Path=/api/inferdeck/v1; HttpOnly; SameSite=Strict");
        resp.set_content(R"({"ok":true})", "application/json");
    }));

    server.Get(std::string(strict_openai_route(
                   StrictOpenAIRoute::Models).pattern),
               wrap([&](const httplib::Request& req,
                                      httplib::Response& resp) {
        handle_models(req, resp, deps);
    }));
    server.Post(control_api_path("/swap/to/:name"),
                wrap([&](const httplib::Request& req,
                         httplib::Response& resp) {
        handle_swap_to(req, resp, deps, req.path_params.at("name"));
    }));
    server.Get(control_api_pattern("/swap/status"),
               wrap([&](const httplib::Request& req,
                        httplib::Response& resp) {
        handle_swap_status(req, resp, deps);
    }));
    server.Post(control_api_pattern("/swap/cancel"),
                wrap([&](const httplib::Request& req,
                         httplib::Response& resp) {
        handle_swap_cancel(req, resp, deps);
    }));
    server.Post(std::string(strict_openai_route(
                    StrictOpenAIRoute::ChatCompletions).pattern),
                wrap([&](const httplib::Request& req,
                                                 httplib::Response& resp) {
        handle_chat_completions(req, resp, deps);
    }));
    server.Post(std::string(strict_openai_route(
                    StrictOpenAIRoute::Embeddings).pattern),
                wrap([&](const httplib::Request& req,
                                           httplib::Response& resp) {
        handle_embeddings(req, resp, deps);
    }));
    server.Post(std::string(strict_openai_route(
                    StrictOpenAIRoute::Responses).pattern),
                wrap([&](const httplib::Request& req,
                                          httplib::Response& resp) {
        handle_responses(req, resp, deps);
    }));
    server.Post(std::string(strict_openai_route(
                    StrictOpenAIRoute::ImageGenerations).pattern),
                wrap([&](const httplib::Request& req,
                                                        httplib::Response& resp) {
        handle_image_generations(req, resp, deps);
    }));
    server.Post(std::string(strict_openai_route(
                    StrictOpenAIRoute::AudioSpeech).pattern),
                wrap([&](const httplib::Request& req,
                                                   httplib::Response& resp) {
        handle_audio_speech(req, resp, deps);
    }));
    server.Post(std::string(strict_openai_route(
                    StrictOpenAIRoute::AudioTranscriptions).pattern),
                wrap([&](const httplib::Request& req,
                                                           httplib::Response& resp) {
        handle_audio_transcriptions(req, resp, deps);
    }));
    if (cfg.openai_derivative_compatibility_enabled) {
        server.Post(std::string(openai_derivative_route(
                        OpenAIDerivativeRoute::ChatCompletions).pattern),
                    wrap([&](const httplib::Request& req,
                             httplib::Response& resp) {
            handle_chat_completions(req, resp, derivative_deps);
        }));
        server.Post(std::string(openai_derivative_route(
                        OpenAIDerivativeRoute::Responses).pattern),
                    wrap([&](const httplib::Request& req,
                             httplib::Response& resp) {
            handle_responses(req, resp, derivative_deps);
        }));
        server.Post(std::string(openai_derivative_route(
                        OpenAIDerivativeRoute::Embeddings).pattern),
                    wrap([&](const httplib::Request& req,
                             httplib::Response& resp) {
            handle_embeddings(req, resp, derivative_deps);
        }));
        server.Post(std::string(openai_derivative_route(
                        OpenAIDerivativeRoute::ImageGenerations).pattern),
                    wrap([&](const httplib::Request& req,
                             httplib::Response& resp) {
            handle_image_generations(req, resp, derivative_deps);
        }));
    }
    server.Get(control_api_pattern("/media/jobs"),
               wrap([&](const httplib::Request&,
                        httplib::Response& resp) {
        write_json(resp, 200, {{"jobs", media_jobs()}});
    }));
    server.Post(control_api_pattern("/media/jobs/([0-9]+)/cancel"),
                wrap([&](const httplib::Request& req,
                         httplib::Response& resp) {
        auto result = cancel_media_job(static_cast<std::uint64_t>(std::stoull(req.matches[1].str())));
        if (!result) {
            write_error(resp, result.error().code == foundation::ErrorCode::NotFound ? 404 : 409,
                        "media_cancel_failed", result.error().message);
            return;
        }
        write_json(resp, 200, {{"ok", true}});
    }));
    server.Get(control_api_pattern("/metrics"),
               wrap([&](const httplib::Request&,
                        httplib::Response& resp) {
        resp.set_content(
            MetricsBuilder::build_live(metrics, gpu, uptime_seconds()).dump(),
            "application/json");
    }));
    server.Get(control_api_pattern("/stats/history"),
               wrap([&](const httplib::Request&,
                        httplib::Response& resp) {
        resp.set_content(MetricsBuilder::build_history(stats_db, 100).dump(),
                         "application/json");
    }));
    server.Get(control_api_pattern("/health"),
               wrap([&](const httplib::Request&,
                        httplib::Response& resp) {
        resp.set_content(nlohmann::json{
                             {"ok", true},
                             {"db_healthy", stats_db.healthy()},
                             {"version", inferdeck::app::build_version},
                             {"build_revision", inferdeck::app::build_revision},
                             {"build_dirty", inferdeck::app::build_dirty}}.dump(),
                         "application/json");
    }));
    auto request_config_reload = [] {
        g_reload.store(true);
        LOG_INFO("config_reload_requested", "validated active profile will be applied");
        return foundation::Ok();
    };
    auto config_repository = std::make_shared<ConfigRepository>(
        config_path, config_selection.active_path,
        [](const std::string& text) { return validate_config_text(text); },
        request_config_reload);
    DashboardDeps dash_deps{
        deps, gpu, cfg.log_file, "data/pricing.json", config_path.string(),
        config_selection.active_path.string(), running_config_revision,
        config_selection.using_active,
        config_selection.fallback_reason,
        [](const std::string& text) { return validate_config_text(text); },
        request_config_reload,
        &model_store,
        uptime_seconds,
        &profile_benchmark,
        &profile_benchmark_scheduler,
        std::move(config_repository)};
    register_dashboard_routes(server, dash_deps, wrap);
    if (data_cors.handles_options() || control_cors.handles_options()) {
        server.Options(".*", [](const httplib::Request&,
                                 httplib::Response& resp) {
            resp.status = 204;
        });
    }
    server.Get(R"(^/$)", [&](const httplib::Request& req, httplib::Response& resp) {
        write_dashboard_file(resp, dashboard_static_dir, req.path);
    });
    server.Get(R"(^/(?!api(?:/|$)|v1(?:/|$)|compat(?:/|$)).*)", [&](const httplib::Request& req, httplib::Response& resp) {
        write_dashboard_file(resp, dashboard_static_dir, req.path);
    });

    std::atomic<bool> reload_monitor_stop{false};
    std::thread reload_monitor([&] {
        while (!reload_monitor_stop.load()) {
            if (g_reload.load() || g_stop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{350});
                if (reload_monitor_stop.load()) return;
                if (g_default_model_loading.load()) continue;
                server.stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }
    });

    std::thread default_model_loader;
    bool listen_ok = false;
    const bool bound = server.bind_to_port(cfg.host.c_str(), cfg.port);
    if (bound) {
        if (!cfg.default_model.empty() && registry.has(cfg.default_model)) {
            g_default_model_loading.store(true);
            default_model_loader = std::thread(
                [&coordinator, default_model = cfg.default_model] {
                    LOG_INFO("default_model_load_begin", "name={}", default_model);
                    model::AcquireSlotOptions options;
                    options.timeout = std::chrono::minutes{5};
                    options.priority = -100;
                    options.cancelled = [] {
                        return g_stop.load() || g_reload.load();
                    };
                    options.prepare = [&coordinator, default_model] {
                        return coordinator.swap_to_cancellable(
                            default_model, std::chrono::minutes{5}, [] {
                                return g_stop.load() || g_reload.load();
                            });
                    };
                    auto slot = coordinator.acquire_slot(default_model, options);
                    auto loaded = slot
                        ? coordinator.release_slot(default_model, *slot)
                        : foundation::Err<void>(slot.error().code, slot.error().message);
                    if (loaded) {
                        LOG_INFO("default_model_load_complete", "name={}", default_model);
                    } else {
                        LOG_ERROR("default_model_load_failed", "name={} error={}",
                                  default_model, loaded.error().message);
                    }
                    g_default_model_loading.store(false);
                });
        }
        LOG_INFO("server_listening", "host={} port={}", cfg.host, cfg.port);
        listen_ok = server.listen_after_bind();
    }
    reload_monitor_stop.store(true);
    if (reload_monitor.joinable()) reload_monitor.join();
    g_server = nullptr;

    ShutdownStateMachine shutdown;
    const auto shutdown_phase = shutdown.run(
        [&] { coordinator.request_swap_cancel(); },
        [&] {
            return !g_default_model_loading.load() &&
                !swap_tracker.snapshot().swapping &&
                coordinator.active_request_count() == 0;
        },
        ShutdownStateMachine::Clock::now() + std::chrono::seconds{120});
    if (shutdown_phase == ShutdownPhase::TimedOut) {
        LOG_FATAL("shutdown_deadline_exceeded",
                  "backend lifecycle did not quiesce within 120 seconds");
        foundation::Logger::instance().shutdown();
        std::quick_exit(2);
    }
    if (default_model_loader.joinable()) default_model_loader.join();
    swap_tracker.join();

    stats_stop.store(true);
    events.close_all();
    if (stats_thread.joinable()) stats_thread.join();

    if (!listen_ok) {
        LOG_ERROR("server_failed", "could not bind {}:{}", cfg.host, cfg.port);
        return 1;
    }

    if (!cfg.state_file.empty()) {
        if (auto m = coordinator.get_loaded_model()) {
            auto persisted = persist_state(cfg.state_file, *m);
            if (!persisted) {
                LOG_WARN("state_persist_failed", "error={}",
                         persisted.error().message);
            }
        }
    }
    if (g_reload.load()) {
        LOG_INFO("server_reloading", "graceful runtime reload");
        return runtime_reload_result;
    }
    LOG_INFO("server_stopped", "graceful shutdown");
    return 0;
}

int main(int argc, char** argv) {
    fs::path config_path = inferdeck::gateway::default_config_path();
    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument == "-c" || argument == "--config") {
            if (i + 1 < argc) config_path = argv[++i];
        } else if (argument == "-h" || argument == "--help") {
            std::cout << "Usage: " << argv[0] << " [-c config.yml]\n";
            return 0;
        } else if (argument == "-v" || argument == "--version") {
            std::cout << inferdeck::app::build_identity() << "\n";
            return 0;
        }
    }

    std::set_terminate(my_terminate_handler);
#ifdef _WIN32
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(0, CrashHandler);
#endif
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    while (true) {
        const int result = run_gateway(config_path);
        if (result != runtime_reload_result) return result;
    }
}
