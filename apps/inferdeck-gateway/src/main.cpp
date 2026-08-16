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
std::atomic<bool> g_default_model_loading{false};
httplib::Server* g_server = nullptr;
std::once_flag g_llama_init_once;
constexpr int runtime_reload_result = 75;
constexpr std::string_view gateway_version = "0.6.1";

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
    if (g_server && !g_default_model_loading.load()) g_server->stop();
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
        executable_dir() / "static",
        fs::current_path() / "apps" / "inferdeck-gateway" / "static",
        fs::current_path() / "static",
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

inferdeck::llama_wrapper::LlamaCppConfig make_llama_config(
    const inferdeck::gateway::GatewayConfig& cfg,
    const model::ModelInfo& info,
    const inferdeck::optimize::ProfileCandidate* candidate = nullptr) {
    inferdeck::llama_wrapper::LlamaCppConfig result;
    result.n_batch = candidate
        ? candidate->n_batch
        : info.n_batch.value_or(cfg.n_batch);
    result.n_ubatch = candidate
        ? candidate->n_ubatch
        : info.n_ubatch.value_or(cfg.n_ubatch);
    result.use_mmap = cfg.use_mmap;
    result.use_mlock = cfg.use_mlock;
    result.n_gpu_layers =
        info.n_gpu_layers.has_value() ? info.n_gpu_layers : cfg.n_gpu_layers;
    result.flash_attn =
        candidate ? candidate->flash_attention : cfg.flash_attn;
    result.kv_offload = cfg.kv_offload;
    result.op_offload = cfg.op_offload;
    result.cache_type_k =
        candidate
            ? candidate->cache_type_k
            : (info.cache_type_k.empty() ? cfg.cache_type_k : info.cache_type_k);
    result.cache_type_v =
        candidate
            ? candidate->cache_type_v
            : (info.cache_type_v.empty() ? cfg.cache_type_v : info.cache_type_v);
    result.mtp_enabled = info.mtp_enabled;
    result.mtp_draft_tokens = info.mtp_draft_tokens;
    result.mtp_p_min = info.mtp_p_min;
    result.mtp_max_active_requests = candidate
        ? candidate->mtp_max_active_requests
        : info.mtp_max_active_requests;
    result.swa_full = cfg.swa_full;
    result.truncate_prompt = cfg.truncate_prompt;
    result.reasoning_format =
        info.reasoning_format.empty() ? "auto" : info.reasoning_format;
    result.sampling = info.sampling;
    if (!info.chat_template_path.empty()) {
        result.chat_template =
            inferdeck::gateway::read_text_file(info.chat_template_path);
    }
    return result;
}

std::string normalized_answer(std::string text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const unsigned char character : text) {
        if (std::isalnum(character)) {
            normalized.push_back(
                static_cast<char>(std::tolower(character)));
        }
    }
    return normalized;
}

foundation::Result<inferdeck::gateway::ProfileBenchmarkTrialMetrics>
run_profile_benchmark_trial(
    const inferdeck::gateway::GatewayConfig& cfg,
    observability::GpuTelemetry& gpu,
    const model::ModelInfo& registered,
    const inferdeck::optimize::ProfileCandidate& candidate,
    const std::vector<inferdeck::gateway::ProfileBenchmarkPrompt>& prompts,
    const std::atomic<bool>& cancel,
    const inferdeck::gateway::ProfileBenchmarkProgress& progress) {
    using Metrics = inferdeck::gateway::ProfileBenchmarkTrialMetrics;
    model::ModelInfo info = registered;
    info.context_size = candidate.context_per_slot;
    info.n_slots = candidate.slots;
    info.mtp_max_active_requests = candidate.mtp_max_active_requests;
    auto runtime = std::make_unique<inferdeck::llama_wrapper::LlamaCppModel>(
        info, make_llama_config(cfg, info, &candidate));
    const double baseline_vram = gpu.latest().vram_mb;
    std::atomic<double> peak_vram{baseline_vram};
    const auto sample_vram = [&] {
        const double sample = gpu.latest().vram_mb;
        double current = peak_vram.load();
        while (sample > current &&
               !peak_vram.compare_exchange_weak(current, sample)) {}
    };
    progress("loading", "Loading candidate profile into the selected model");
    const auto load_started = std::chrono::steady_clock::now();
    auto loaded = runtime->load();
    const auto load_finished = std::chrono::steady_clock::now();
    if (!loaded) {
        return foundation::Err<Metrics>(
            loaded.error().code, loaded.error().message);
    }
    const auto unload = [&] {
        (void)runtime->unload();
        for (int sample = 0; sample < 20; ++sample) {
            if (gpu.latest().vram_mb <= baseline_vram + 512.0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    };
    for (int sample = 0; sample < 10 && !cancel.load(); ++sample) {
        sample_vram();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    if (cancel.load()) {
        unload();
        return foundation::Err<Metrics>(
            foundation::ErrorCode::Cancelled,
            "benchmark cancelled after loading the candidate");
    }

    Metrics measured;
    measured.load_ms =
        std::chrono::duration<double, std::milli>(
            load_finished - load_started).count();
    double total_ttft_ms = 0.0;
    progress("quality", "Running fixed-seed quality and latency probes");
    for (std::size_t index = 0; index < prompts.size(); ++index) {
        if (cancel.load()) {
            unload();
            return foundation::Err<Metrics>(
                foundation::ErrorCode::Cancelled,
                "benchmark cancelled during quality probes");
        }
        auto slot = runtime->acquire_slot();
        if (!slot) {
            unload();
            return foundation::Err<Metrics>(
                slot.error().code, slot.error().message);
        }
        model::InferenceRequest request;
        request.messages = {
            {"system",
             "Answer directly and concisely. <|think_off|>"},
            {"user", prompts[index].prompt},
        };
        request.max_tokens = prompts[index].max_tokens;
        request.temperature = 0.0f;
        request.top_p = 1.0f;
        request.top_k = 0;
        request.repeat_penalty = 1.0f;
        request.seed = 4242 + static_cast<int>(index);
        const auto started = std::chrono::steady_clock::now();
        std::atomic<bool> first_token{false};
        std::chrono::steady_clock::time_point first_token_at{};
        auto result = runtime->predict_stream(
            *slot, request,
            [&](const model::InferenceDelta& delta) {
                sample_vram();
                if ((!delta.content.empty() ||
                     !delta.reasoning_text.empty()) &&
                    !first_token.exchange(true)) {
                    first_token_at = std::chrono::steady_clock::now();
                }
                return !cancel.load();
            },
            &cancel);
        (void)runtime->release_slot(*slot);
        if (!result) {
            unload();
            return foundation::Err<Metrics>(
                result.error().code, result.error().message);
        }
        const auto finished = std::chrono::steady_clock::now();
        const double ttft = first_token.load()
            ? std::chrono::duration<double, std::milli>(
                  first_token_at - started).count()
            : std::chrono::duration<double, std::milli>(
                  finished - started).count();
        total_ttft_ms += ttft;
        measured.prompt_tokens += result->prompt_tokens;
        measured.completion_tokens += result->completion_tokens;
        ++measured.quality_total;
        const auto& answer = result->text.empty()
            ? result->reasoning_text
            : result->text;
        const auto output = normalized_answer(answer);
        const auto reference = normalized_answer(prompts[index].reference);
        double score = 0.0;
        if (output == reference) {
            score = 1.0;
            ++measured.quality_passes;
        } else if (!reference.empty() &&
                   output.find(reference) != std::string::npos) {
            score = 0.75;
            ++measured.quality_passes;
        }
        measured.quality_score += score;
        measured.output_samples.push_back(
            prompts[index].id + ": " +
            answer.substr(0, std::min<std::size_t>(
                answer.size(), 160)));
    }
    if (measured.quality_total > 0) {
        measured.quality_score /=
            static_cast<double>(measured.quality_total);
        measured.average_time_to_first_token_ms =
            total_ttft_ms / static_cast<double>(measured.quality_total);
    }

    progress("speed", "Measuring sustained single-slot output throughput");
    auto speed_slot = runtime->acquire_slot();
    if (!speed_slot) {
        unload();
        return foundation::Err<Metrics>(
            speed_slot.error().code, speed_slot.error().message);
    }
    model::InferenceRequest speed_request;
    speed_request.messages = {
        {"system", "Answer directly and concisely. <|think_off|>"},
        {"user",
         "Output the lowercase word benchmark exactly 128 times, "
         "separated by single spaces. Do not add punctuation or any "
         "other text."},
    };
    speed_request.max_tokens = 192;
    speed_request.temperature = 0.0f;
    speed_request.top_p = 1.0f;
    speed_request.top_k = 0;
    speed_request.repeat_penalty = 1.0f;
    speed_request.seed = 7001;
    auto speed_result = runtime->predict_stream(
        *speed_slot, speed_request,
        [&](const model::InferenceDelta&) {
            sample_vram();
            return !cancel.load();
        },
        &cancel);
    (void)runtime->release_slot(*speed_slot);
    if (!speed_result) {
        unload();
        return foundation::Err<Metrics>(
            speed_result.error().code, speed_result.error().message);
    }
    measured.prompt_tokens += speed_result->prompt_tokens;
    measured.completion_tokens += speed_result->completion_tokens;
    measured.prompt_tokens_per_second = speed_result->prompt_duration_ms > 0.0f
        ? static_cast<double>(speed_result->prompt_tokens) * 1000.0 /
            static_cast<double>(speed_result->prompt_duration_ms)
        : 0.0;
    measured.average_tokens_per_second =
        speed_result->tokens_per_second;

    std::vector<int> concurrency_levels;
    if (candidate.slots >= 2) concurrency_levels.push_back(2);
    if (candidate.slots >= 4) {
        concurrency_levels.push_back(4);
    } else if (candidate.slots > 2) {
        concurrency_levels.push_back(candidate.slots);
    }
    if (concurrency_levels.empty()) concurrency_levels.push_back(1);
    for (const int parallel_slots : concurrency_levels) {
        progress(
            "parallelism",
            "Measuring " + std::to_string(parallel_slots) +
                " concurrent requests and MTP drafting");
        const auto parallel_started = std::chrono::steady_clock::now();
        std::vector<std::future<foundation::Result<model::InferenceResult>>> futures;
        futures.reserve(static_cast<std::size_t>(parallel_slots));
        for (int index = 0; index < parallel_slots; ++index) {
            futures.push_back(std::async(
                std::launch::async,
                [&, index, parallel_slots] {
                    auto slot = runtime->acquire_slot();
                    if (!slot) {
                        return foundation::Err<model::InferenceResult>(
                            slot.error().code, slot.error().message);
                    }
                    model::InferenceRequest request;
                    request.messages = {
                        {"system",
                         "Answer directly and concisely. <|think_off|>"},
                        {"user",
                         "Output the lowercase word benchmark exactly 96 times, "
                         "separated by single spaces. Do not add punctuation or "
                         "any other text. Concurrency " +
                             std::to_string(parallel_slots) + " request " +
                             std::to_string(index + 1) + "."},
                    };
                    request.max_tokens = 144;
                    request.temperature = 0.0f;
                    request.top_p = 1.0f;
                    request.top_k = 0;
                    request.repeat_penalty = 1.0f;
                    request.seed = 9000 + parallel_slots * 10 + index;
                    auto result = runtime->predict_stream(
                        *slot, request,
                        [&](const model::InferenceDelta&) {
                            sample_vram();
                            return !cancel.load();
                        },
                        &cancel);
                    (void)runtime->release_slot(*slot);
                    return result;
                }));
        }
        inferdeck::gateway::ProfileBenchmarkConcurrencyMetrics concurrency;
        concurrency.requests = parallel_slots;
        int parallel_tokens = 0;
        double longest_generation_ms = 0.0;
        double request_tps_total = 0.0;
        for (auto& future : futures) {
            auto result = future.get();
            if (!result) {
                unload();
                return foundation::Err<Metrics>(
                    result.error().code, result.error().message);
            }
            measured.prompt_tokens += result->prompt_tokens;
            measured.completion_tokens += result->completion_tokens;
            parallel_tokens += result->completion_tokens;
            longest_generation_ms = std::max(
                longest_generation_ms,
                static_cast<double>(result->generation_duration_ms));
            request_tps_total += result->tokens_per_second;
            concurrency.mtp_drafted_tokens += result->mtp_drafted_tokens;
            concurrency.mtp_accepted_tokens += result->mtp_accepted_tokens;
            if (result->mtp_drafted_tokens > 0) ++concurrency.mtp_requests;
        }
        const auto parallel_finished = std::chrono::steady_clock::now();
        const double parallel_wall_seconds = std::chrono::duration<double>(
            parallel_finished - parallel_started).count();
        const double parallel_seconds = longest_generation_ms > 0.0
            ? longest_generation_ms / 1000.0
            : parallel_wall_seconds;
        concurrency.aggregate_tokens_per_second = parallel_seconds > 0.0
            ? static_cast<double>(parallel_tokens) / parallel_seconds
            : 0.0;
        concurrency.average_request_tokens_per_second =
            request_tps_total / static_cast<double>(parallel_slots);
        measured.parallel_tokens_per_second =
            concurrency.aggregate_tokens_per_second;
        measured.concurrency.push_back(std::move(concurrency));
    }
    sample_vram();
    measured.peak_vram_mb = peak_vram.load();
    unload();
    return foundation::Ok(std::move(measured));
}

} // namespace

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
                     cfg.default_model, cfg.anthropic_model_aliases,
                     cfg.voice_session_grace_ms,
                     &metrics, &stats_db, &events, &swap_tracker,
                     &maintenance_resource};
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

    AuthConfig ac;
    ac.required = cfg.auth_required;
    ac.token = cfg.auth_token;
    AuthMiddleware auth(ac);
    CorsMiddleware cors(cfg.cors_origins);

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
            if (g_stop.load() || g_reload.load()) {
                resp.set_header("Retry-After", "1");
                write_error(resp, 503, "server_stopping",
                            "gateway shutdown or reload is in progress");
                return;
            }
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
        uptime_seconds,
        &profile_benchmark,
        &profile_benchmark_scheduler};
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
                        return coordinator.swap_to(default_model);
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
    if (default_model_loader.joinable()) default_model_loader.join();
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
