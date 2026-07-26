#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
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

#include "config.hpp"
#include "foundation/event_bus.hpp"
#include "foundation/json_utils.hpp"
#include "foundation/logging.hpp"
#include "foundation/path_utils.hpp"
#include "gateway/anthropic_routes.hpp"
#include "gateway/auth.hpp"
#include "gateway/cors.hpp"
#include "gateway/dashboard_routes.hpp"
#include "gateway/metrics_builder.hpp"
#include "gateway/media_routes.hpp"
#include "gateway/model_store.hpp"
#include "gateway/openai_routes.hpp"
#include "gateway/routes.hpp"
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
httplib::Server* g_server = nullptr;
std::once_flag g_llama_init_once;
constexpr int runtime_reload_result = 75;
constexpr std::string_view gateway_version = "0.5.3";

std::string config_revision(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

FILE* open_crash_log() noexcept {
    FILE* file = nullptr;
#ifdef _WIN32
    (void)fopen_s(&file, "logs/crash.log", "a");
#else
    file = fopen("logs/crash.log", "a");
#endif
    return file;
}

void my_terminate_handler() {
    std::cerr << "=== std::terminate called ===" << std::endl;
    FILE* f = open_crash_log();
    if (f) {
        fprintf(f, "=== std::terminate called ===\n");
    }
    try {
        throw;
    } catch (const std::exception& e) {
        std::cerr << "Terminate: std::exception: " << e.what() << std::endl;
        if (f) fprintf(f, "Terminate: std::exception: %s\n", e.what());
    } catch (...) {
        std::cerr << "Terminate: unknown exception" << std::endl;
        if (f) fprintf(f, "Terminate: unknown exception\n");
    }
    if (f) fclose(f);
    std::abort();
}

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

foundation::Result<void> persist_state(const std::string& path,
                                       const std::string& model) {
    if (path.empty()) return foundation::Ok();
    const auto expanded = foundation::expand_user_path(fs::path(path));
    const auto parent = expanded.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        fs::create_directories(parent, error);
        if (error) {
            return foundation::Err<void>(
                foundation::ErrorCode::IoError,
                "failed to create state directory: " + error.message());
        }
    }
    return foundation::save_json_file(
        expanded, nlohmann::json{{"loaded_model", model}}, true);
}

std::string mime_type(const fs::path& path) {
    const auto ext = path.extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    return "application/octet-stream";
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

fs::path executable_dir() {
#ifdef _WIN32
    char module_path[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, module_path, static_cast<DWORD>(sizeof(module_path))) > 0) {
        return fs::path(module_path).parent_path();
    }
#endif
    return fs::current_path();
}

fs::path find_dashboard_static_dir() {
    std::vector<fs::path> candidates = {
        fs::current_path() / "apps" / "inferdeck-gateway" / "static",
        fs::current_path() / "static",
        executable_dir() / "static",
        executable_dir() / "dashboard"
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate / "index.html", ec)) return candidate;
    }
    return candidates.front();
}

void write_dashboard_file(httplib::Response& resp, const fs::path& static_dir, const std::string& request_path) {
    fs::path relative = request_path == "/" ? fs::path("index.html") : fs::path(request_path.substr(1));
    std::error_code ec;
    fs::path target = fs::weakly_canonical(static_dir / relative, ec);
    fs::path root = fs::weakly_canonical(static_dir, ec);
    if (ec || !inferdeck::foundation::is_path_within(root, target) ||
        !fs::exists(target, ec) || fs::is_directory(target, ec)) {
        target = static_dir / "index.html";
    }
    const auto body = read_file(target);
    if (body.empty()) {
        resp.status = 404;
        resp.set_content("InferDeck dashboard has not been built. Run npm run build in apps/dashboard.", "text/plain");
        return;
    }
    resp.status = 200;
    resp.set_content(body, mime_type(target));
}

#ifdef _WIN32
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ex) {
    DWORD code = ex->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP || code == 0x40010006 || code == 0xE06D7363) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    FILE* f = open_crash_log();
    if (f) {
        fprintf(f, "=== CRASH ===\n");
        fprintf(f, "code=0x%08lX addr=%p\n", code, ex->ExceptionRecord->ExceptionAddress);
        CONTEXT* ctx = ex->ContextRecord;
        fprintf(f, "rip=%p rsp=%p rbp=%p\n", (void*)ctx->Rip, (void*)ctx->Rsp, (void*)ctx->Rbp);
        fprintf(f, "rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p\n",
                (void*)ctx->Rbx, (void*)ctx->Rcx, (void*)ctx->Rdx,
                (void*)ctx->Rsi, (void*)ctx->Rdi);
        fprintf(f, "r8=%p r9=%p r10=%p r11=%p r12=%p r13=%p r14=%p r15=%p\n",
                (void*)ctx->R8, (void*)ctx->R9, (void*)ctx->R10, (void*)ctx->R11,
                (void*)ctx->R12, (void*)ctx->R13, (void*)ctx->R14, (void*)ctx->R15);
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset = ctx->Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx->Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx->Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        HANDLE proc = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();
        SymInitialize(proc, NULL, TRUE);
        for (int i = 0; i < 30; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, ctx, NULL,
                             SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
            if (frame.AddrPC.Offset == 0) break;
            DWORD64 disp = 0;
            SYMBOL_INFO* sym = (SYMBOL_INFO*)malloc(sizeof(SYMBOL_INFO) + 256);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            if (SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym)) {
                fprintf(f, "  #%d 0x%llx %s+0x%llx\n", i, frame.AddrPC.Offset, sym->Name, disp);
            } else {
                IMAGEHLP_MODULE64 module{};
                module.SizeOfStruct = sizeof(module);
                if (SymGetModuleInfo64(proc, frame.AddrPC.Offset, &module)) {
                    fprintf(f, "  #%d 0x%llx %s!(no symbol)\n", i, frame.AddrPC.Offset, module.ModuleName);
                } else {
                    fprintf(f, "  #%d 0x%llx (no symbol)\n", i, frame.AddrPC.Offset);
                }
            }
            free(sym);
        }
        SymCleanup(proc);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

} // namespace

int run_gateway(const fs::path& config_path) {
    using namespace inferdeck;
    using namespace inferdeck::foundation;
    using namespace inferdeck::gateway;

    g_stop.store(false);
    g_reload.store(false);
    const auto config_selection = load_config_with_active(config_path);
    auto cfg = config_selection.config;
    const auto running_config_revision =
        config_revision(read_text_file(config_selection.loaded_path.string()));
    foundation::LogConfig lc;
    lc.level = parse_log_level(cfg.log_level);
    if (!cfg.log_file.empty()) lc.log_file = cfg.log_file;
    foundation::Logger::instance().initialize(lc);

    LOG_INFO("startup", "inferdeck-gateway {} starting", gateway_version);
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
        std::cerr << "DEBUG: About to call llama_backend_init()" << std::endl;
        llama_backend_init();
        const char* sys_info = llama_print_system_info();
        std::cerr << "DEBUG: llama_print_system_info returned" << std::endl;
        if (sys_info) {
            LOG_INFO("llama_system_info", "{}", sys_info);
            std::cerr << "DEBUG: sys_info = " << sys_info << std::endl;
        }
    });

    model::ModelRegistry registry;
    registry.register_factory("llama_cpp", [cfg](const model::ModelInfo& info) -> std::unique_ptr<model::IBackend> {
        llama_wrapper::LlamaCppConfig lc;
        lc.n_batch = cfg.n_batch;
        lc.n_ubatch = cfg.n_ubatch;
        lc.use_mmap = cfg.use_mmap;
        lc.use_mlock = cfg.use_mlock;
        lc.n_gpu_layers = info.n_gpu_layers.has_value() ? info.n_gpu_layers : cfg.n_gpu_layers;
        lc.flash_attn = cfg.flash_attn;
        lc.kv_offload = cfg.kv_offload;
        lc.op_offload = cfg.op_offload;
        lc.cache_type_k = cfg.cache_type_k;
        lc.cache_type_v = cfg.cache_type_v;
        lc.swa_full = cfg.swa_full;
        lc.truncate_prompt = cfg.truncate_prompt;
        lc.reasoning_format = info.reasoning_format.empty() ? "auto" : info.reasoning_format;
        lc.sampling = info.sampling;  // per-model merged over global (issue #42)
        // Optional per-model chat-template override (.jinja). Empty path keeps the
        // template embedded in the GGUF. Used to fix the Qwen3.6 multi-step tool
        // calling crash ("No user query found in messages.").
        if (!info.chat_template_path.empty()) {
            lc.chat_template = read_text_file(info.chat_template_path);
            if (lc.chat_template.empty()) {
                LOG_ERROR("chat_template_missing",
                          "chat_template_path set but file empty/unreadable for {}: {} -- falling back to embedded template",
                          info.name, info.chat_template_path);
            } else {
                LOG_INFO("chat_template_override",
                         "loaded chat template for {} from {} ({} bytes)",
                         info.name, info.chat_template_path, lc.chat_template.size());
            }
        }
        return std::make_unique<llama_wrapper::LlamaCppModel>(info, lc);
    });
    native_runtimes::register_factories(registry);
    for (const auto& m : cfg.models) {
        registry.register_model(m);
        LOG_INFO("model_registered", "name={} vram_mb={} n_slots={}",
                 m.name, m.vram_required_mb, m.n_slots);
    }
    LOG_INFO("factory_set", "LlamaCppModel factory installed");

    model::BackendCoordinator coordinator(registry);
    coordinator.set_max_queue_size(static_cast<std::size_t>(std::max(1, cfg.max_queue_size)));
    if (cfg.vram_budget_mb > 0) {
        coordinator.set_vram_budget(cfg.vram_budget_mb, cfg.vram_safety_margin_mb);
    }
    ModelStore model_store(cfg.model_store_root, cfg.model_store_hf_token, coordinator);

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
    GatewayDeps deps{coordinator, "15", cfg.auto_swap,
                     cfg.default_model, cfg.anthropic_model_aliases,
                     &metrics, &stats_db, &events, &swap_tracker};

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
    server.new_task_queue = [] {
        return new httplib::ThreadPool(std::max(32u, std::thread::hardware_concurrency() * 2u));
    };
    g_server = &server;
    const auto dashboard_static_dir = find_dashboard_static_dir();

    RouteWrapper wrap = [&](httplib::Server::Handler handler) -> httplib::Server::Handler {
        return [&, handler](const httplib::Request& req,
                            httplib::Response& resp) {
            LOG_INFO("http_request_begin", "method={} path={}", req.method, req.path);
            cors.apply(req, resp);
            std::string auth_header = header_value(req, "Authorization");
            if (auth_header.empty()) {
                // Anthropic SDK clients (e.g. Claude Code) authenticate via x-api-key.
                const std::string api_key = header_value(req, "x-api-key");
                if (!api_key.empty()) auth_header = "Bearer " + api_key;
            }
            if (auth.required() && !auth.check(auth_header)) {
                resp.set_header("WWW-Authenticate", "Bearer");
                write_error(resp, 401, "unauthorized", "valid Bearer token required");
                return;
            }
            try {
                handler(req, resp);
                const int logged_status = resp.status > 0 ? resp.status : 200;
                LOG_INFO("http_request_end", "method={} path={} status={}", req.method, req.path, logged_status);
            } catch (const std::exception& e) {
                LOG_ERROR("handler_exception", "what={}", e.what());
                if (resp.status == 0) resp.status = 500;
                write_error(resp, resp.status, "internal_error", "request could not be completed");
                const int logged_status = resp.status > 0 ? resp.status : 500;
                LOG_INFO("http_request_end", "method={} path={} status={}", req.method, req.path, logged_status);
            } catch (...) {
                LOG_ERROR("handler_unknown_exception", "");
                if (resp.status == 0) resp.status = 500;
                write_error(resp, resp.status, "internal_error", "unknown exception");
                const int logged_status = resp.status > 0 ? resp.status : 500;
                LOG_INFO("http_request_end", "method={} path={} status={}", req.method, req.path, logged_status);
            }
        };
    };

    server.Get(R"(^/v1/models$)", wrap([&](const httplib::Request& req,
                                      httplib::Response& resp) {
        handle_models(req, resp, deps);
    }));
    server.Post("/v1/swap/to/:name", wrap([&](const httplib::Request& req,
                                              httplib::Response& resp) {
        handle_swap_to(req, resp, deps, req.path_params.at("name"));
    }));
    server.Get(R"(^/v1/swap/status$)", wrap([&](const httplib::Request& req,
                                           httplib::Response& resp) {
        handle_swap_status(req, resp, deps);
    }));
    server.Post(R"(^/v1/swap/cancel$)", wrap([&](const httplib::Request& req,
                                            httplib::Response& resp) {
        handle_swap_cancel(req, resp, deps);
    }));
    server.Post(R"(^/v1/chat/completions$)", wrap([&](const httplib::Request& req,
                                                 httplib::Response& resp) {
        handle_chat_completions(req, resp, deps);
    }));
    server.Post(R"(^/v1/embeddings$)", wrap([&](const httplib::Request& req,
                                           httplib::Response& resp) {
        handle_embeddings(req, resp, deps);
    }));
    server.Post(R"(^/v1/responses$)", wrap([&](const httplib::Request& req,
                                          httplib::Response& resp) {
        handle_responses(req, resp, deps);
    }));
    server.Post(R"(^/v1/images/generations$)", wrap([&](const httplib::Request& req,
                                                        httplib::Response& resp) {
        handle_image_generations(req, resp, deps);
    }));
    server.Post(R"(^/v1/audio/speech$)", wrap([&](const httplib::Request& req,
                                                   httplib::Response& resp) {
        handle_audio_speech(req, resp, deps);
    }));
    server.Post(R"(^/v1/audio/transcriptions$)", wrap([&](const httplib::Request& req,
                                                           httplib::Response& resp) {
        handle_audio_transcriptions(req, resp, deps);
    }));
    server.Get(R"(^/api/media/jobs$)", wrap([&](const httplib::Request&,
                                                httplib::Response& resp) {
        write_json(resp, 200, {{"jobs", media_jobs()}});
    }));
    server.Post(R"(^/api/media/jobs/([0-9]+)/cancel$)", wrap([&](const httplib::Request& req,
                                                                 httplib::Response& resp) {
        auto result = cancel_media_job(static_cast<std::uint64_t>(std::stoull(req.matches[1].str())));
        if (!result) {
            write_error(resp, result.error().code == foundation::ErrorCode::NotFound ? 404 : 409,
                        "media_cancel_failed", result.error().message);
            return;
        }
        write_json(resp, 200, {{"ok", true}});
    }));
    server.Post(R"(^/v1/messages$)", wrap([&](const httplib::Request& req,
                                         httplib::Response& resp) {
        handle_anthropic_messages(req, resp, deps);
    }));
    server.Post(R"(^/v1/messages/count_tokens$)", wrap([&](const httplib::Request& req,
                                                      httplib::Response& resp) {
        handle_anthropic_count_tokens(req, resp, deps);
    }));
    server.Get(R"(^/v1/metrics$)", wrap([&](const httplib::Request&,
                                       httplib::Response& resp) {
        resp.set_content(
            MetricsBuilder::build_live(metrics, gpu, uptime_seconds()).dump(),
            "application/json");
    }));
    server.Get(R"(^/v1/stats/history$)", wrap([&](const httplib::Request&,
                                             httplib::Response& resp) {
        resp.set_content(MetricsBuilder::build_history(stats_db, 100).dump(),
                         "application/json");
    }));
    server.Get(R"(^/v1/health$)", wrap([&](const httplib::Request&,
                                      httplib::Response& resp) {
        resp.set_content(MetricsBuilder::build_health(metrics, gpu, stats_db).dump(),
                         "application/json");
    }));
    DashboardDeps dash_deps{
        deps, gpu, cfg.log_file, "data/pricing.json", config_path.string(),
        config_selection.active_path.string(), running_config_revision,
        config_selection.using_active,
        config_selection.fallback_reason,
        [](const std::string& text) { return validate_config_text(text); },
        [] {
            g_reload.store(true);
            LOG_INFO("config_reload_requested", "validated active profile will be applied");
            return foundation::Ok();
        },
        &model_store,
        uptime_seconds};
    register_dashboard_routes(server, dash_deps, wrap);
    if (cors.handles_options()) {
        server.Options(".*", [&](const httplib::Request& req,
                                  httplib::Response& resp) {
            cors.apply(req, resp);
            resp.status = 204;
        });
    }
    server.Get(R"(^/$)", [&](const httplib::Request& req, httplib::Response& resp) {
        cors.apply(req, resp);
        write_dashboard_file(resp, dashboard_static_dir, req.path);
    });
    server.Get(R"(^/(?!api(?:/|$)|v1(?:/|$)).*)", [&](const httplib::Request& req, httplib::Response& resp) {
        cors.apply(req, resp);
        write_dashboard_file(resp, dashboard_static_dir, req.path);
    });

    std::atomic<bool> reload_monitor_stop{false};
    std::thread reload_monitor([&] {
        while (!reload_monitor_stop.load()) {
            if (g_reload.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{350});
                if (!reload_monitor_stop.load()) server.stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }
    });

    LOG_INFO("server_listening", "host={} port={}", cfg.host, cfg.port);
    const bool listen_ok = server.listen(cfg.host.c_str(), cfg.port);
    reload_monitor_stop.store(true);
    if (reload_monitor.joinable()) reload_monitor.join();
    g_server = nullptr;

    if (g_reload.load()) {
        coordinator.drain_active(std::chrono::seconds{120});
    }

    stats_stop.store(true);
    events.close_all();
    if (stats_thread.joinable()) stats_thread.join();

    const auto swap_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{120};
    while (swap_tracker.snapshot().swapping &&
           std::chrono::steady_clock::now() < swap_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

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
            std::cout << "inferdeck-gateway " << gateway_version << "\n";
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
