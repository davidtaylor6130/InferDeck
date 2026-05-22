#include "routes/Dashboard.hpp"

#include "RuntimeActivity.hpp"
#include "llama_cpp/LlamaEngine.hpp"
#include "llama_cpp/LlamaServerManager.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using json = nlohmann::json;

namespace inferdeck::gateway::routes {
namespace {

void JsonResponse(httplib::Response& resp, const json& body, int status = 200) {
    resp.status = status;
    resp.set_content(body.dump(), "application/json");
}

void JsonError(httplib::Response& resp, int status, const std::string& code, const std::string& message) {
    JsonResponse(resp, json{{"error", {{"code", code}, {"message", message}}}}, status);
}

std::uint64_t ToUnixSeconds(std::filesystem::file_time_type value) {
    auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(std::chrono::system_clock::to_time_t(system_time));
}

std::string IsoNow() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::int64_t UptimeSeconds(GatewayStartTime started_at) {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at).count();
}

std::string NormalizeId(std::string id) {
    constexpr size_t latest_suffix_len = 7;
    if (id.size() > latest_suffix_len && id.compare(id.size() - latest_suffix_len, latest_suffix_len, ":latest") == 0) {
        id = id.substr(0, id.size() - latest_suffix_len);
    }
    for (auto& c : id) {
        if (c == ' ' || c == '_') c = '-';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return id;
}

std::string StripKnownQuantSuffix(std::string stem) {
    std::string lower = NormalizeId(stem);
    static const std::vector<std::string> suffixes = {
        "-q4-k-m", "-q4-k-s", "-q4-0", "-q5-k-m", "-q5-k-s", "-q5-0",
        "-q6-k", "-q8-0", "-iq2-xs", "-iq2-xxs", "-iq3-xs", "-iq3-xxs",
        "-f16", "-f32", "-bf16", "-mxfp4"
    };
    for (const auto& suffix : suffixes) {
        if (lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return stem.substr(0, stem.size() - suffix.size());
        }
    }
    return stem;
}

std::vector<std::filesystem::path> ScanModelFiles(const std::string& model_directory, const std::string& configured_model) {
    std::vector<std::filesystem::path> files;
    auto add_if_model = [&](const std::filesystem::path& path) {
        if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) return;
        auto ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto name = path.filename().string();
        auto lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if ((ext == ".gguf" || ext == ".bin" || ext == ".ggml") && lower_name.find("mmproj") == std::string::npos) {
            files.push_back(path);
        }
    };

    if (!configured_model.empty()) add_if_model(configured_model);
    if (!model_directory.empty() && std::filesystem::exists(model_directory)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(model_directory)) {
            if (entry.is_regular_file()) add_if_model(entry.path());
        }
    }

    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

json ModelJson(const std::filesystem::path& path, bool loaded) {
    auto stem = path.stem().string();
    auto clean = NormalizeId(StripKnownQuantSuffix(stem));
    auto full = NormalizeId(stem);
    json aliases = json::array({clean, clean + ":latest", full, full + ":latest", stem});
    json model = {
        {"id", clean},
        {"name", stem},
        {"path", path.string()},
        {"object", "model"},
        {"owned_by", "inferdeck"},
        {"loaded", loaded},
        {"aliases", aliases},
        {"details", {{"format", path.extension().string() == ".gguf" ? "gguf" : path.extension().string()}, {"backend", "llama.cpp"}, {"parameter_size", "100000 ctx"}}}
    };
    if (std::filesystem::exists(path)) {
        model["size"] = static_cast<std::uint64_t>(std::filesystem::file_size(path));
        model["modified_at"] = ToUnixSeconds(std::filesystem::last_write_time(path));
    }
    return model;
}

std::string ResolveModelPath(const std::string& requested, const ServerConfig& config) {
    if (requested.empty()) return "";
    std::filesystem::path direct(requested);
    if (std::filesystem::exists(direct)) return direct.string();

    auto wanted = NormalizeId(requested);
    for (const auto& path : ScanModelFiles(config.model_directory, config.model_path)) {
        auto stem = path.stem().string();
        auto clean = NormalizeId(StripKnownQuantSuffix(stem));
        auto full = NormalizeId(stem);
        if (wanted == clean || wanted == full || wanted == NormalizeId(path.filename().string())) {
            return path.string();
        }
    }
    return "";
}

#ifdef _WIN32
std::uint64_t FileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

json CpuUsageJson() {
    static std::uint64_t last_idle = 0;
    static std::uint64_t last_kernel = 0;
    static std::uint64_t last_user = 0;

    FILETIME idle_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        return {{"name", "Windows host CPU"}, {"utilization", nullptr}};
    }

    const auto idle = FileTimeToU64(idle_time);
    const auto kernel = FileTimeToU64(kernel_time);
    const auto user = FileTimeToU64(user_time);
    double utilization = 0.0;
    if (last_kernel != 0 || last_user != 0) {
        const auto system_delta = (kernel - last_kernel) + (user - last_user);
        const auto idle_delta = idle - last_idle;
        if (system_delta > 0) {
            utilization = 100.0 * static_cast<double>(system_delta - idle_delta) / static_cast<double>(system_delta);
            utilization = std::clamp(utilization, 0.0, 100.0);
        }
    }

    last_idle = idle;
    last_kernel = kernel;
    last_user = user;

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return {
        {"name", "Windows host CPU"},
        {"utilization", utilization},
        {"logicalProcessors", static_cast<unsigned int>(info.dwNumberOfProcessors)}
    };
}
#endif

json DiskJson() {
    std::filesystem::path data_dir = std::filesystem::current_path() / "data";
    std::error_code ec;
    auto space = std::filesystem::space(data_dir, ec);
    if (ec) {
        return {{"path", data_dir.string()}, {"free", nullptr}, {"total", nullptr}, {"used", nullptr}, {"percentage", nullptr}};
    }
    const auto used = space.capacity > space.available ? space.capacity - space.available : 0;
    const double percentage = space.capacity > 0 ? (static_cast<double>(used) * 100.0 / static_cast<double>(space.capacity)) : 0.0;
    return {
        {"path", data_dir.string()},
        {"free", static_cast<std::uint64_t>(space.available)},
        {"total", static_cast<std::uint64_t>(space.capacity)},
        {"used", static_cast<std::uint64_t>(used)},
        {"percentage", percentage}
    };
}

json HardwareJson() {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    auto gpu_info = engine.GetGpuInfo();
    json hardware;
    hardware["available"] = true;
    hardware["provider"] = "windows";
    hardware["timestamp"] = IsoNow();
    hardware["gpu"] = {
        {"name", gpu_info.name.empty() ? "AMD GPU (Vulkan)" : gpu_info.name},
        {"backend", "llama.cpp b9276 Vulkan"},
        {"driverVersion", "b9276 Vulkan runtime"},
        {"utilization", nullptr},
        {"temperature", nullptr},
        {"power", nullptr},
        {"fanSpeed", nullptr},
        {"memoryTotal", gpu_info.memory_total == 0 ? json(nullptr) : json(gpu_info.memory_total)},
        {"memoryFree", gpu_info.memory_free == 0 ? json(nullptr) : json(gpu_info.memory_free)},
        {"memoryUsed", gpu_info.memory_total > gpu_info.memory_free ? json(gpu_info.memory_total - gpu_info.memory_free) : json(nullptr)},
        {"memoryPercent", gpu_info.memory_total > 0 ? json(((gpu_info.memory_total - gpu_info.memory_free) * 100.0) / gpu_info.memory_total) : json(nullptr)}
    };

#ifdef _WIN32
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        hardware["memory"] = {
            {"total", static_cast<std::uint64_t>(mem.ullTotalPhys)},
            {"free", static_cast<std::uint64_t>(mem.ullAvailPhys)},
            {"used", static_cast<std::uint64_t>(mem.ullTotalPhys - mem.ullAvailPhys)},
            {"percentage", static_cast<int>(mem.dwMemoryLoad)}
        };
    }
#else
    hardware["memory"] = {{"percentage", nullptr}};
#endif
#ifdef _WIN32
    hardware["cpu"] = CpuUsageJson();
#else
    hardware["cpu"] = {{"utilization", nullptr}, {"name", "Host CPU"}};
#endif
    hardware["disk"] = DiskJson();
    return hardware;
}

json LlamaServiceJson(const ServerConfig& config, GatewayStartTime started_at) {
    auto& manager = inferdeck::core::LlamaServerManager::Get();
    auto& engine = inferdeck::core::LlamaEngine::Get();
    bool running = manager.IsRunning();
    return {
        {"id", "llama-server"},
        {"name", "llama.cpp"},
        {"kind", "llama_cpp"},
        {"status", running ? "running" : "stopped"},
        {"managed", true},
        {"pid", running ? json(manager.GetPid()) : json(nullptr)},
        {"baseUrl", "http://127.0.0.1:" + std::to_string(manager.GetPort())},
        {"version", "b9276-vulkan"},
        {"backend", "Vulkan"},
        {"currentModel", engine.GetModelName()},
        {"currentModelPath", manager.GetCurrentModelPath().empty() ? config.model_path : manager.GetCurrentModelPath()},
        {"uptime", running ? json(UptimeSeconds(started_at)) : json(0)},
        {"lastHealthcheckAt", IsoNow()}
    };
}

json GatewayServiceJson(const ServerConfig& config, GatewayStartTime started_at) {
    return {
        {"id", "gateway"},
        {"name", "Gateway"},
        {"kind", "gateway"},
        {"status", "running"},
        {"managed", true},
        {"pid", nullptr},
        {"baseUrl", "http://" + config.host + ":" + std::to_string(config.apiPort)},
        {"dashboardUrl", "http://" + config.host + ":" + std::to_string(config.dashboardPort)},
        {"version", "1.0.0"},
        {"uptime", UptimeSeconds(started_at)},
        {"lastHealthcheckAt", IsoNow()}
    };
}

json MetricsJson() {
    auto stats = inferdeck::core::LlamaEngine::Get().GetStats();
    auto summary = inferdeck::gateway::RuntimeActivity::Get().SummaryJson();
    auto queue = inferdeck::gateway::RuntimeActivity::Get().QueueJson();
    return {
        {"total_requests", stats.total_requests},
        {"successful_requests", stats.successful_requests},
        {"failed_requests", stats.failed_requests},
        {"avg_latency_ms", stats.avg_latency_ms},
        {"max_latency_ms", stats.max_latency_ms},
        {"min_latency_ms", stats.min_latency_ms == 999999.0f ? 0.0f : stats.min_latency_ms},
        {"tokens_generated", stats.tokens_generated},
        {"tokens_processed", stats.tokens_processed},
        {"jobs_today", summary["jobsToday"]},
        {"total_tokens", summary["totalTokens"]},
        {"counters", {
            {"inferdeck.requests.total", stats.total_requests},
            {"inferdeck.requests.success", stats.successful_requests},
            {"inferdeck.requests.failed", stats.failed_requests},
            {"inferdeck.tokens.generated", stats.tokens_generated},
            {"inferdeck.tokens.processed", stats.tokens_processed},
            {"inferdeck.jobs.today", summary["jobsToday"]}
        }},
        {"gauges", {{"queue.pending", queue["queued"]}, {"queue.running", queue["running"]}, {"queue.failed", queue["failed"]}}},
        {"histograms", {{"inferdeck.latency_ms", {{"min", stats.min_latency_ms == 999999.0f ? 0.0f : stats.min_latency_ms}, {"max", stats.max_latency_ms}, {"avg", stats.avg_latency_ms}, {"count", stats.successful_requests}, {"sum", stats.avg_latency_ms * stats.successful_requests}}}}}
    };
}

std::uint64_t DirectorySize(const std::filesystem::path& path) {
    std::uint64_t total = 0;
    if (!std::filesystem::exists(path)) return total;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path, std::filesystem::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) total += static_cast<std::uint64_t>(entry.file_size());
        }
    } catch (...) {}
    return total;
}

json StorageJson(const ServerConfig& config) {
    std::filesystem::path data_dir = std::filesystem::current_path() / "data";
    std::filesystem::path logs_dir = std::filesystem::current_path() / "logs";
    std::error_code ec;
    auto data_space = std::filesystem::space(data_dir, ec);
    json storage = {
        {"dataDirectory", data_dir.string()},
        {"logsDirectory", logs_dir.string()},
        {"logSize", DirectorySize(logs_dir)},
        {"dbSize", DirectorySize(data_dir)},
        {"storage", "filesystem + SQLite WAL"},
        {"modelDirectory", config.model_directory},
        {"freeSpace", ec ? nullptr : json(static_cast<std::uint64_t>(data_space.available))},
        {"totalSpace", ec ? nullptr : json(static_cast<std::uint64_t>(data_space.capacity))}
    };
    return storage;
}

void EnsureAdminPost(const httplib::Request& req, httplib::Response& resp) {
    if (req.method != "POST") {
        JsonError(resp, 405, "method_not_allowed", "Control endpoints require POST.");
    }
}

std::vector<std::string> ReadTail(const std::filesystem::path& path, std::size_t limit) {
    std::vector<std::string> lines;
    std::ifstream file(path);
    if (!file.is_open()) return lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
        if (lines.size() > limit) lines.erase(lines.begin());
    }
    return lines;
}

json ParseLogLine(const std::string& line, const std::string& service) {
    std::string level = "info";
    auto lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("error") != std::string::npos) level = "error";
    else if (lower.find("warn") != std::string::npos) level = "warn";
    return {
        {"timestamp", IsoNow()},
        {"level", level},
        {"service", service},
        {"message", line},
        {"jobId", nullptr}
    };
}

} // namespace

void HandleDashboardHealth(const httplib::Request&, httplib::Response& resp, const ServerConfig& config, GatewayStartTime started_at) {
    auto& engine = inferdeck::core::LlamaEngine::Get();
    auto& manager = inferdeck::core::LlamaServerManager::Get();
    JsonResponse(resp, {
        {"status", engine.IsInitialized() && manager.IsRunning() ? "healthy" : "degraded"},
        {"version", "1.0.0"},
        {"uptime", UptimeSeconds(started_at)},
        {"engine_ready", engine.IsInitialized()},
        {"gateway", GatewayServiceJson(config, started_at)},
        {"llama", LlamaServiceJson(config, started_at)}
    });
}

void HandleDashboardStatus(const httplib::Request&, httplib::Response& resp, const ServerConfig& config, GatewayStartTime started_at) {
    auto metrics = MetricsJson();
    auto& activity = inferdeck::gateway::RuntimeActivity::Get();
    auto queue = activity.QueueJson();
    auto summary = activity.SummaryJson();
    JsonResponse(resp, {
        {"status", "ok"},
        {"mode", {{"mode", "ai"}, {"queuePaused", queue["pausedByAdmin"]}}},
        {"queue", queue},
        {"hardware", HardwareJson()},
        {"storage", StorageJson(config)},
        {"summary", summary},
        {"metrics", metrics},
        {"metricsSamples", activity.SamplesJson()},
        {"services", json::array({GatewayServiceJson(config, started_at), LlamaServiceJson(config, started_at)})}
    });
}

void HandleDashboardModels(const httplib::Request&, httplib::Response& resp, const ServerConfig& config) {
    auto current = inferdeck::core::LlamaServerManager::Get().GetCurrentModelPath();
    if (current.empty()) current = config.model_path;
    json models = json::array();
    std::unordered_set<std::string> seen;
    for (const auto& path : ScanModelFiles(config.model_directory, config.model_path)) {
        auto key = NormalizeId(path.string());
        if (seen.count(key)) continue;
        seen.insert(key);
        bool loaded = false;
        if (!current.empty()) {
            try {
                loaded = std::filesystem::equivalent(std::filesystem::path(current), path);
            } catch (...) {
                loaded = NormalizeId(current) == NormalizeId(path.string());
            }
        }
        models.push_back(ModelJson(path, loaded));
    }
    JsonResponse(resp, {{"models", models}, {"current", inferdeck::core::LlamaEngine::Get().GetModelName()}, {"backends", {{"llama_cpp", "ready"}}}});
}

void HandleDashboardRunningModels(const httplib::Request&, httplib::Response& resp) {
    auto current = inferdeck::core::LlamaServerManager::Get().GetCurrentModelPath();
    json running = json::array();
    if (!current.empty() && std::filesystem::exists(current)) running.push_back(ModelJson(current, true));
    JsonResponse(resp, {{"running", running}});
}

void HandleDashboardLoadModel(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config) {
    EnsureAdminPost(req, resp);
    if (resp.status >= 400) return;
    try {
        auto body = req.body.empty() ? json::object() : json::parse(req.body);
        std::string requested = body.value("model", body.value("name", ""));
        std::string model_path = ResolveModelPath(requested, config);
        if (model_path.empty()) {
            JsonError(resp, 404, "model_not_found", "No GGUF model matched '" + requested + "'.");
            return;
        }
        auto& engine = inferdeck::core::LlamaEngine::Get();
        bool ok = engine.IsInitialized()
            ? engine.SwitchModel(model_path)
            : engine.Initialize(model_path, config.precision, config.n_gpu_layers, config.context_size);
        if (!ok) {
            JsonError(resp, 500, "model_load_failed", "llama.cpp could not load the requested model.");
            return;
        }
        JsonResponse(resp, {{"ok", true}, {"model", ModelJson(model_path, true)}, {"status", "running"}});
    } catch (const std::exception& e) {
        JsonError(resp, 400, "bad_request", e.what());
    }
}

void HandleDashboardUnloadModel(const httplib::Request& req, httplib::Response& resp) {
    EnsureAdminPost(req, resp);
    if (resp.status >= 400) return;
    inferdeck::core::LlamaEngine::Get().Shutdown();
    JsonResponse(resp, {{"ok", true}, {"status", "stopped"}});
}

void HandleDashboardRescanModels(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config) {
    EnsureAdminPost(req, resp);
    if (resp.status >= 400) return;
    JsonResponse(resp, {{"ok", true}, {"count", ScanModelFiles(config.model_directory, config.model_path).size()}});
}

void HandleDashboardServices(const httplib::Request&, httplib::Response& resp, const ServerConfig& config, GatewayStartTime started_at) {
    JsonResponse(resp, {{"services", json::array({GatewayServiceJson(config, started_at), LlamaServiceJson(config, started_at)})}});
}

void HandleDashboardStartService(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config) {
    EnsureAdminPost(req, resp);
    if (resp.status >= 400) return;
    if (!inferdeck::core::LlamaEngine::Get().Initialize(config.model_path, config.precision, config.n_gpu_layers, config.context_size)) {
        JsonError(resp, 500, "start_failed", "llama.cpp failed to start.");
        return;
    }
    JsonResponse(resp, {{"ok", true}, {"status", "running"}});
}

void HandleDashboardStopService(const httplib::Request& req, httplib::Response& resp) {
    EnsureAdminPost(req, resp);
    if (resp.status >= 400) return;
    inferdeck::core::LlamaEngine::Get().Shutdown();
    JsonResponse(resp, {{"ok", true}, {"status", "stopped"}});
}

void HandleDashboardRestartService(const httplib::Request& req, httplib::Response& resp, const ServerConfig& config) {
    EnsureAdminPost(req, resp);
    if (resp.status >= 400) return;
    auto current = inferdeck::core::LlamaServerManager::Get().GetCurrentModelPath();
    if (current.empty()) current = config.model_path;
    if (current.empty()) {
        JsonError(resp, 404, "model_not_configured", "No current or configured model is available to restart.");
        return;
    }
    auto& engine = inferdeck::core::LlamaEngine::Get();
    if (engine.IsInitialized()) {
        engine.Shutdown();
    }
    bool ok = engine.Initialize(current, config.precision, config.n_gpu_layers, config.context_size);
    if (!ok) {
        JsonError(resp, 500, "restart_failed", "llama.cpp failed to restart.");
        return;
    }
    JsonResponse(resp, {{"ok", true}, {"status", "running"}, {"model", engine.GetModelName()}});
}

void HandleDashboardJobs(const httplib::Request&, httplib::Response& resp) {
    JsonResponse(resp, {{"jobs", inferdeck::gateway::RuntimeActivity::Get().JobsJson()}});
}

std::string JobIdFromMatch(const httplib::Request& req) {
    return req.matches.size() > 1 ? req.matches[1].str() : "";
}

void HandleDashboardJobDetail(const httplib::Request& req, httplib::Response& resp) {
    auto job = inferdeck::gateway::RuntimeActivity::Get().JobJson(JobIdFromMatch(req));
    if (job.is_null()) {
        JsonError(resp, 404, "job_not_found", "Job was not found in recent runtime history.");
        return;
    }
    JsonResponse(resp, job);
}

void HandleDashboardJobEvents(const httplib::Request& req, httplib::Response& resp) {
    JsonResponse(resp, {{"events", inferdeck::gateway::RuntimeActivity::Get().JobEventsJson(JobIdFromMatch(req))}});
}

void HandleDashboardJobResult(const httplib::Request& req, httplib::Response& resp) {
    JsonResponse(resp, {{"result", inferdeck::gateway::RuntimeActivity::Get().JobResultJson(JobIdFromMatch(req))}});
}

void HandleDashboardCancelJob(const httplib::Request& req, httplib::Response& resp) {
    EnsureAdminPost(req, resp);
    if (resp.status >= 400) return;
    inferdeck::gateway::RuntimeActivity::Get().CancelJob(JobIdFromMatch(req));
    JsonResponse(resp, {{"ok", true}, {"id", JobIdFromMatch(req)}, {"status", "cancelled"}});
}

void HandleDashboardRetryJob(const httplib::Request& req, httplib::Response& resp) {
    EnsureAdminPost(req, resp);
    if (resp.status >= 400) return;
    auto retry_id = inferdeck::gateway::RuntimeActivity::Get().RetryJob(JobIdFromMatch(req));
    if (retry_id.empty()) {
        JsonError(resp, 404, "job_not_found", "Job was not found in recent runtime history.");
        return;
    }
    JsonResponse(resp, {{"ok", true}, {"id", retry_id}, {"status", "queued"}});
}

void HandleDashboardQueueAction(const httplib::Request& req, httplib::Response& resp, const std::string& action) {
    EnsureAdminPost(req, resp);
    if (resp.status >= 400) return;
    auto& activity = inferdeck::gateway::RuntimeActivity::Get();
    if (action == "pause") activity.SetQueuePaused(true);
    if (action == "resume") activity.SetQueuePaused(false);
    if (action == "clear-failed") activity.ClearFailedJobs();
    JsonResponse(resp, {{"ok", true}, {"action", action}, {"queue", activity.QueueJson()}});
}

void HandleDashboardLogs(const httplib::Request& req, httplib::Response& resp) {
    std::size_t limit = 500;
    if (req.has_param("limit")) {
        try { limit = static_cast<std::size_t>(std::stoul(req.get_param_value("limit"))); } catch (...) {}
    }
    limit = std::clamp<std::size_t>(limit, 1, 1000);
    json logs = json::array();
    for (const auto& line : ReadTail("logs/gateway.log", limit / 2 + 1)) logs.push_back(ParseLogLine(line, "gateway"));
    for (const auto& line : ReadTail("logs/llama-server.err.log", limit / 2 + 1)) logs.push_back(ParseLogLine(line, "llama_cpp"));
    for (const auto& item : inferdeck::gateway::RuntimeActivity::Get().LogsJson(limit / 2 + 1)) logs.push_back(item);
    if (logs.size() > limit) {
        logs.erase(logs.begin(), logs.begin() + static_cast<json::difference_type>(logs.size() - limit));
    }
    JsonResponse(resp, {{"logs", logs}});
}

void HandleDashboardEvents(const httplib::Request&, httplib::Response& resp) {
    resp.status = 200;
    resp.set_header("Cache-Control", "no-cache");
    resp.set_header("Connection", "keep-alive");
    resp.set_content("event: connected\ndata: {\"ok\":true}\n\nevent: heartbeat\ndata: {}\n\n", "text/event-stream");
}

} // namespace inferdeck::gateway::routes
