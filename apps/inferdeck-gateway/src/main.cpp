#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#endif

#include "config.hpp"
#include "foundation/logging.hpp"
#include "gateway/auth.hpp"
#include "gateway/cors.hpp"
#include "gateway/metrics_builder.hpp"
#include "gateway/routes.hpp"
#include "httplib.h"
#include "llama.h"
#include "llama_cpp_wrapper/llama_cpp_model.hpp"
#include "model/backend_coordinator.hpp"
#include "model/model_registry.hpp"
#include "observability/gpu_telemetry.hpp"
#include "observability/metrics.hpp"
#include "observability/stats_db.hpp"
#include "scheduler/scheduler.hpp"

namespace fs = std::filesystem;

namespace {

namespace model = inferdeck::model;
namespace observability = inferdeck::observability;

std::atomic<bool> g_stop{false};
httplib::Server* g_server = nullptr;

void my_terminate_handler() {
    std::cerr << "=== std::terminate called ===" << std::endl;
    FILE* f = fopen("logs/crash.log", "a");
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

void persist_state(const std::string& path, const std::string& model) {
    if (path.empty()) return;
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;
    f << "loaded_model: " << model << "\n";
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

nlohmann::json build_dashboard_health(
    const observability::Metrics& metrics,
    const observability::GpuTelemetry& gpu,
    const observability::StatsDb& stats_db,
    std::int64_t uptime_seconds) {
    const auto live = gpu.latest();
    return {
        {"status", stats_db.healthy() ? "healthy" : "degraded"},
        {"version", "2.0.0"},
        {"uptime", uptime_seconds},
        {"db_healthy", stats_db.healthy()},
        {"db_path", stats_db.path()},
        {"telemetry", {{"available", live.available}, {"provider", live.provider}, {"reason", live.reason}}},
        {"requests", metrics.total_requests()}
    };
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
            {"details", {
                {"backend", "llama.cpp"},
                {"format", "gguf"},
                {"parameter_size", std::to_string(info.context_size) + " ctx"}
            }}
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
            {"details", {{"backend", "llama.cpp"}, {"format", "gguf"}}}
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
            {"priority", 50},
            {"client", "OpenAI compatible"},
            {"model", row.model},
            {"resourceClass", "gpu_llm"},
            {"createdAt", timestamp},
            {"promptTokens", row.prompt_tokens},
            {"completionTokens", row.completion_tokens},
            {"totalTokens", row.prompt_tokens + row.completion_tokens},
            {"durationMs", row.duration_ms},
            {"httpStatus", row.status_code}
        });
    }
    return {{"jobs", jobs}};
}

nlohmann::json build_dashboard_status(
    model::BackendCoordinator& coordinator,
    const observability::Metrics& metrics,
    const observability::GpuTelemetry& gpu,
    const observability::StatsDb& stats_db,
    std::int64_t uptime_seconds) {
    auto hardware = gpu_hardware_json(gpu.latest());
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
        usage.push_back({
            {"model", row.model},
            {"requests", row.requests},
            {"successfulRequests", row.successful_requests},
            {"promptTokens", row.prompt_tokens},
            {"completionTokens", row.completion_tokens},
            {"totalTokens", row.prompt_tokens + row.completion_tokens},
            {"prompt_tokens", row.prompt_tokens},
            {"completion_tokens", row.completion_tokens},
            {"total_tokens", row.prompt_tokens + row.completion_tokens},
            {"peakTokensPerSecond", row.peak_tokens_per_second},
            {"lastTimestampUnixMs", row.last_timestamp_unix_ms}
        });
    }

    nlohmann::json monthly = nlohmann::json::array();
    for (const auto& row : stats_db.monthly_usage()) {
        monthly.push_back({
            {"bucket", row.bucket},
            {"model", row.model},
            {"promptTokens", row.prompt_tokens},
            {"completionTokens", row.completion_tokens},
            {"totalTokens", row.total_tokens},
            {"requests", row.requests},
            {"successfulRequests", row.successful_requests}
        });
    }

    auto loaded = coordinator.get_loaded_model();
    auto model_json = build_dashboard_models(coordinator);
    return {
        {"status", "ok"},
        {"mode", {{"mode", "ai"}, {"queuePaused", false}}},
        {"queue", {
            {"queued", 0},
            {"running", coordinator.active_request_count()},
            {"paused", 0},
            {"failed", 0},
            {"gpuLocked", coordinator.active_request_count() > 0},
            {"lockOwner", loaded.value_or("")}
        }},
        {"hardware", hardware},
        {"hardwareSamples", nlohmann::json::array()},
        {"summary", {
            {"jobsToday", requests},
            {"totalTokens", prompt_tokens + completion_tokens},
            {"promptTokens", prompt_tokens},
            {"completionTokens", completion_tokens},
            {"avgLatencyMs", metrics.total_requests() > 0 ? metrics.total_duration_ms() / static_cast<double>(metrics.total_requests()) : 0.0}
        }},
        {"metrics", {
            {"total_requests", metrics.total_requests()},
            {"total_swaps", metrics.total_swaps()},
            {"tokens_processed", prompt_tokens},
            {"tokens_generated", completion_tokens},
            {"total_tokens", prompt_tokens + completion_tokens},
            {"avg_tokens_per_second", metrics.avg_tokens_per_second()}
        }},
        {"tokenUsage", usage},
        {"monthlyTokenUsage", monthly},
        {"models", model_json["models"]},
        {"running", model_json["running"]},
        {"services", nlohmann::json::array({
            {{"id", "gateway"}, {"name", "Gateway"}, {"kind", "gateway"}, {"status", "running"}, {"baseUrl", "http://127.0.0.1:11434"}},
            {{"id", "llama-cpp"}, {"name", "llama.cpp in-process"}, {"kind", "llama_cpp"}, {"status", loaded ? "running" : "stopped"}, {"managed", true}}
        })},
        {"uptime", uptime_seconds}
    };
}

void write_dashboard_file(httplib::Response& resp, const fs::path& static_dir, const std::string& request_path) {
    fs::path relative = request_path == "/" ? fs::path("index.html") : fs::path(request_path.substr(1));
    std::error_code ec;
    fs::path target = fs::weakly_canonical(static_dir / relative, ec);
    fs::path root = fs::weakly_canonical(static_dir, ec);
    if (target.string().rfind(root.string(), 0) != 0 || !fs::exists(target, ec) || fs::is_directory(target, ec)) {
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
    FILE* f = fopen("logs/crash.log", "a");
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

    std::set_terminate(my_terminate_handler);

#ifdef _WIN32
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(0, CrashHandler);
#endif

    LOG_INFO("startup", "inferdeck-gateway 2.0.0 starting");
    LOG_INFO("config", "loaded from {}", config_path.string());
    LOG_INFO("vulkan_test", "About to initialize llama backend");
    std::cerr << "DEBUG: About to call llama_backend_init()" << std::endl;

    llama_backend_init();
    const char* sys_info = llama_print_system_info();
    std::cerr << "DEBUG: llama_print_system_info returned" << std::endl;
    if (sys_info) {
        LOG_INFO("llama_system_info", "{}", sys_info);
        std::cerr << "DEBUG: sys_info = " << sys_info << std::endl;
    }

    model::ModelRegistry registry;
    registry.set_factory([cfg](const model::ModelInfo& info) -> std::unique_ptr<model::IModel> {
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
        lc.reasoning_format = info.reasoning_format.empty() ? "auto" : info.reasoning_format;
        return std::make_unique<llama_wrapper::LlamaCppModel>(info, lc);
    });
    for (const auto& m : cfg.models) {
        registry.register_model(m);
        LOG_INFO("model_registered", "name={} vram_mb={} n_slots={}",
                 m.name, m.vram_required_mb, m.n_slots);
    }
    LOG_INFO("factory_set", "LlamaCppModel factory installed");

    model::BackendCoordinator coordinator(registry);
    scheduler::Scheduler scheduler(coordinator);

    observability::Metrics metrics;
    observability::GpuTelemetry gpu;
    observability::StatsDb stats_db(cfg.stats_db_path);
    const auto started_at = std::chrono::steady_clock::now();

    gpu.set_helper_path(cfg.adlx_helper_path);
    gpu.set_poll_interval(std::chrono::milliseconds(cfg.telemetry_poll_ms));
    gpu.start();
    LOG_INFO("gpu_telemetry_started", "provider=windows_pdh_dxgi poll_ms={}",
             cfg.telemetry_poll_ms);

    GatewayDeps deps{coordinator, scheduler, "15", cfg.auto_swap, &metrics, &stats_db};

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
    server.new_task_queue = [] {
        return new httplib::ThreadPool(std::max(32u, std::thread::hardware_concurrency() * 2u));
    };
    g_server = &server;
    const auto dashboard_static_dir = find_dashboard_static_dir();

    auto wrap = [&](auto handler) {
        return [&, handler](const httplib::Request& req,
                            httplib::Response& resp) {
            LOG_INFO("http_request_begin", "method={} path={}", req.method, req.path);
            cors.apply(resp);
            if (auth.required() && !auth.check(header_value(req, "Authorization"))) {
                resp.status = 401;
                resp.set_header("WWW-Authenticate", "Bearer");
                write_json(resp, 401, {{"error", {{"code", "unauthorized"},
                                                  {"message", "valid Bearer token required"}}}});
                return;
            }
            try {
                handler(req, resp);
                const int logged_status = resp.status > 0 ? resp.status : 200;
                LOG_INFO("http_request_end", "method={} path={} status={}", req.method, req.path, logged_status);
            } catch (const std::exception& e) {
                LOG_ERROR("handler_exception", "what={}", e.what());
                if (resp.status == 0) resp.status = 500;
                write_json(resp, resp.status, nlohmann::json{{"error", {{"code", "internal_error"}, {"message", e.what()}}}});
                const int logged_status = resp.status > 0 ? resp.status : 500;
                LOG_INFO("http_request_end", "method={} path={} status={}", req.method, req.path, logged_status);
            } catch (...) {
                LOG_ERROR("handler_unknown_exception", "");
                if (resp.status == 0) resp.status = 500;
                write_json(resp, resp.status, nlohmann::json{{"error", {{"code", "internal_error"}, {"message", "unknown exception"}}}});
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
    server.Post(R"(^/v1/chat/completions$)", wrap([&](const httplib::Request& req,
                                                 httplib::Response& resp) {
        handle_chat_completions(req, resp, deps);
    }));
    server.Get(R"(^/v1/metrics$)", wrap([&](const httplib::Request& req,
                                       httplib::Response& resp) {
        resp.set_content(
            MetricsBuilder::build_live(metrics, gpu, uptime_seconds()).dump(),
            "application/json");
    }));
    server.Get(R"(^/v1/stats/history$)", wrap([&](const httplib::Request& req,
                                             httplib::Response& resp) {
        resp.set_content(MetricsBuilder::build_history(stats_db, 100).dump(),
                         "application/json");
    }));
    server.Get(R"(^/v1/health$)", wrap([&](const httplib::Request& req,
                                      httplib::Response& resp) {
        resp.set_content(MetricsBuilder::build_health(metrics, gpu, stats_db).dump(),
                         "application/json");
    }));
    server.Get(R"(^/api/health$)", wrap([&](const httplib::Request& req,
                                       httplib::Response& resp) {
        (void)req;
        resp.set_content(build_dashboard_health(metrics, gpu, stats_db, uptime_seconds()).dump(),
                         "application/json");
    }));
    server.Get(R"(^/api/status$)", wrap([&](const httplib::Request& req,
                                       httplib::Response& resp) {
        (void)req;
        resp.set_content(build_dashboard_status(coordinator, metrics, gpu, stats_db, uptime_seconds()).dump(),
                         "application/json");
    }));
    server.Get(R"(^/api/models$)", wrap([&](const httplib::Request& req,
                                      httplib::Response& resp) {
        (void)req;
        resp.set_content(build_dashboard_models(coordinator).dump(), "application/json");
    }));
    server.Get(R"(^/api/models/running$)", wrap([&](const httplib::Request& req,
                                              httplib::Response& resp) {
        (void)req;
        resp.set_content(nlohmann::json{{"running", build_dashboard_models(coordinator)["running"]}}.dump(),
                         "application/json");
    }));
    server.Post(R"(^/api/models/load$)", wrap([&](const httplib::Request& req,
                                            httplib::Response& resp) {
        auto body = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
        const std::string model_name = body.value("model", body.value("name", ""));
        if (model_name.empty()) {
            write_error(resp, 400, "missing_model", "request body must include model");
            return;
        }
        if (!coordinator.registry().has(model_name)) {
            write_error(resp, 404, "model_not_found", "model not registered: " + model_name);
            return;
        }
        const auto current = coordinator.get_loaded_model();
        const auto start = std::chrono::steady_clock::now();
        auto result = coordinator.swap_to_cancellable(model_name);
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (!result) {
            observability::SwapRecord rec{std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count(),
                current.value_or(""), model_name, elapsed, false, result.error().message};
            metrics.record_swap(rec);
            stats_db.record_swap({rec.timestamp_unix_ms, rec.from_model, rec.to_model,
                                  rec.duration_ms, rec.success, rec.error});
            write_error(resp, 500, "swap_failed", result.error().message);
            return;
        }
        observability::SwapRecord rec{std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count(),
            current.value_or(""), model_name, elapsed, true, ""};
        metrics.record_swap(rec);
        stats_db.record_swap({rec.timestamp_unix_ms, rec.from_model, rec.to_model,
                              rec.duration_ms, rec.success, rec.error});
        resp.set_content(nlohmann::json{{"ok", true}, {"status", "running"}, {"model", model_name}}.dump(),
                         "application/json");
    }));
    server.Post(R"(^/api/models/unload$)", wrap([&](const httplib::Request& req,
                                              httplib::Response& resp) {
        (void)req;
        auto result = coordinator.unload_current();
        if (!result) {
            write_error(resp, 500, "unload_failed", result.error().message);
            return;
        }
        resp.set_content(nlohmann::json{{"ok", true}, {"status", "stopped"}}.dump(),
                         "application/json");
    }));
    server.Get(R"(^/api/jobs$)", wrap([&](const httplib::Request& req,
                                    httplib::Response& resp) {
        (void)req;
        resp.set_content(build_dashboard_jobs(stats_db, 100).dump(), "application/json");
    }));
    server.Get(R"(^/api/services$)", wrap([&](const httplib::Request& req,
                                        httplib::Response& resp) {
        (void)req;
        auto loaded = coordinator.get_loaded_model();
        resp.set_content(nlohmann::json{{"services", nlohmann::json::array({
            {{"id", "gateway"}, {"name", "Gateway"}, {"kind", "gateway"}, {"status", "running"}, {"managed", true}},
            {{"id", "llama-cpp"}, {"name", "llama.cpp in-process"}, {"kind", "llama_cpp"}, {"status", loaded ? "running" : "stopped"}, {"managed", true}},
            {{"id", "telemetry"}, {"name", "Hardware telemetry"}, {"kind", "observability"}, {"status", gpu.latest().available ? "running" : "degraded"}, {"managed", true}}
        })}}.dump(), "application/json");
    }));
    server.Post(R"(^/api/(queue|modes|services)/(.*)$)", wrap([&](const httplib::Request& req,
                                                               httplib::Response& resp) {
        (void)req;
        resp.set_content(nlohmann::json{{"ok", true}, {"status", "acknowledged"}}.dump(),
                         "application/json");
    }));
    server.Get(R"(^/api/logs$)", wrap([&](const httplib::Request& req,
                                    httplib::Response& resp) {
        std::size_t limit = 250;
        if (req.has_param("limit")) {
            try { limit = std::clamp<std::size_t>(std::stoul(req.get_param_value("limit")), 1, 1000); } catch (...) {}
        }
        std::ifstream file(cfg.log_file.empty() ? "logs/gateway.log" : cfg.log_file);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
            if (lines.size() > limit) lines.erase(lines.begin());
        }
        nlohmann::json logs = nlohmann::json::array();
        for (const auto& item : lines) {
            logs.push_back({{"timestamp", ""}, {"level", "info"}, {"service", "gateway"}, {"message", item}});
        }
        resp.set_content(nlohmann::json{{"logs", logs}}.dump(), "application/json");
    }));
    server.Get(R"(^/api/events/stream$)", [&](const httplib::Request& req,
                                         httplib::Response& resp) {
        (void)req;
        LOG_INFO("http_request_begin", "method=GET path=/api/events/stream");
        cors.apply(resp);
        resp.status = 204;
        resp.set_header("Cache-Control", "no-cache");
        LOG_INFO("http_request_end", "method=GET path=/api/events/stream status=204");
    });
    if (cors.handles_options()) {
        server.Options(".*", [&](const httplib::Request& req,
                                  httplib::Response& resp) {
            cors.apply(resp);
            resp.status = 204;
        });
    }
    server.Get(R"(^/$)", [&](const httplib::Request& req, httplib::Response& resp) {
        cors.apply(resp);
        write_dashboard_file(resp, dashboard_static_dir, req.path);
    });
    server.Get(R"(^/(?!api(?:/|$)|v1(?:/|$)).*)", [&](const httplib::Request& req, httplib::Response& resp) {
        cors.apply(resp);
        write_dashboard_file(resp, dashboard_static_dir, req.path);
    });

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
