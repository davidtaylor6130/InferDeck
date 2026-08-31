#include "gateway/dashboard_routes.hpp"
#include "gateway/config_repository.hpp"
#include "gateway/config_secrets.hpp"

#include "foundation/logging.hpp"
#include "optimize/profile_optimizer.hpp"
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
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
namespace optimize = inferdeck::optimize;

constexpr std::size_t max_dashboard_streams = 64;
std::atomic<std::size_t> dashboard_streams{0};

#include "dashboard_config_yaml.ipp"

struct DashboardStreamLease {
    std::atomic<bool> held{false};

    bool acquire() {
        auto count = dashboard_streams.load();
        while (count < max_dashboard_streams) {
            if (dashboard_streams.compare_exchange_weak(count, count + 1)) {
                held.store(true);
                return true;
            }
        }
        return false;
    }

    void release() {
        bool expected = true;
        if (held.compare_exchange_strong(expected, false)) {
            dashboard_streams.fetch_sub(1);
        }
    }

    ~DashboardStreamLease() {
        release();
    }
};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::vector<std::string> bounded_log_tail(const std::filesystem::path& path,
                                          std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    auto cursor = input.tellg();
    constexpr std::streamoff block_size = 64 * 1024;
    constexpr std::streamoff maximum_bytes = 4 * 1024 * 1024;
    std::string data;
    std::size_t newlines = 0;
    while (cursor > 0 && static_cast<std::streamoff>(data.size()) < maximum_bytes &&
           newlines <= limit) {
        const auto read_size = std::min(
            block_size, static_cast<std::streamoff>(cursor));
        cursor -= read_size;
        std::string block(static_cast<std::size_t>(read_size), '\0');
        input.seekg(cursor);
        input.read(block.data(), read_size);
        newlines += static_cast<std::size_t>(std::count(
            block.begin(), block.end(), '\n'));
        data.insert(0, std::move(block));
    }
    std::deque<std::string> tail;
    std::istringstream lines(data);
    std::string line;
    while (std::getline(lines, line)) {
        tail.push_back(std::move(line));
        if (tail.size() > limit) tail.pop_front();
    }
    return {std::make_move_iterator(tail.begin()),
            std::make_move_iterator(tail.end())};
}

nlohmann::json gpu_hardware_json(const observability::GpuStats& gpu) {
    const double total_mb = gpu.vram_total_mb;
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
    std::unordered_map<std::string, model::ResidencyInfo> residency;
    for (const auto& resident : coordinator.residency()) residency.emplace(resident.name, resident);
    for (const auto& name : coordinator.registry().list()) {
        const auto& info = coordinator.registry().get_info(name);
        const auto resident = residency.find(name);
        models.push_back({
            {"id", name},
            {"name", name},
            {"family", info.family},
            {"runtime", info.runtime},
            {"runtime_available", coordinator.registry().has_factory(info.runtime)},
            {"modality", info.modality},
            {"role", model::to_string(info.role)},
            {"compute", model::to_string(info.compute)},
            {"residency_policy", model::to_string(info.residency)},
            {"admission_pool", info.admission_pool},
            {"concurrency_limit", info.concurrency_limit},
            {"memory_required_mb", info.memory_required_mb},
            {"eviction_eligible", info.eviction_eligible},
            {"capabilities", info.capabilities},
            {"prompt_price_per_million", info.prompt_price_per_million
                ? nlohmann::json(*info.prompt_price_per_million) : nlohmann::json(nullptr)},
            {"cached_prompt_price_per_million", info.cached_prompt_price_per_million
                ? nlohmann::json(*info.cached_prompt_price_per_million) : nlohmann::json(nullptr)},
            {"completion_price_per_million", info.completion_price_per_million
                ? nlohmann::json(*info.completion_price_per_million) : nlohmann::json(nullptr)},
            {"loaded", resident != residency.end()},
            {"primary", resident != residency.end() && resident->second.primary},
            {"context_size", info.context_size},
            {"vram_required_mb", info.vram_required_mb},
            {"n_slots", resident == residency.end() ? info.n_slots : resident->second.slots},
            {"free_slots", resident == residency.end() ? 0 : resident->second.free_slots},
            {"active_requests", resident == residency.end() ? 0 : resident->second.active_requests},
            {"resizing", resident != residency.end() && resident->second.resizing},
            {"has_vision", info.has_vision},
            {"reasoning", {
                {"supported", info.reasoning.supported},
                {"efforts", info.reasoning.efforts},
                {"default", info.reasoning.default_effort},
                {"none_disables", info.reasoning.none_disables},
                {"aliases", info.reasoning.aliases},
            }},
            {"optimization", {
                {"status", info.optimization.status},
                {"measured_at", info.optimization.measured_at},
                {"quality_passes", info.optimization.quality_passes},
                {"quality_total", info.optimization.quality_total},
                {"single_tokens_per_second", info.optimization.single_tokens_per_second},
                {"parallel_tokens_per_second", info.optimization.parallel_tokens_per_second},
            }},
        });
    }
    for (const auto& alias : coordinator.registry().aliases()) {
        const auto info = coordinator.registry().get_info_result(alias.target);
        if (!info) continue;
        const auto resident = residency.find(alias.target);
        models.push_back({
            {"id", alias.name},
            {"name", alias.name},
            {"family", info->family},
            {"runtime", info->runtime},
            {"runtime_available", coordinator.registry().has_factory(info->runtime)},
            {"modality", info->modality},
            {"role", model::to_string(info->role)},
            {"compute", model::to_string(info->compute)},
            {"residency_policy", model::to_string(info->residency)},
            {"admission_pool", info->admission_pool},
            {"concurrency_limit", info->concurrency_limit},
            {"memory_required_mb", info->memory_required_mb},
            {"eviction_eligible", info->eviction_eligible},
            {"capabilities", info->capabilities},
            {"loaded", resident != residency.end()},
            {"primary", resident != residency.end() && resident->second.primary},
            {"context_size", info->context_size},
            {"vram_required_mb", info->vram_required_mb},
            {"n_slots", resident == residency.end() ? info->n_slots : resident->second.slots},
            {"free_slots", resident == residency.end() ? 0 : resident->second.free_slots},
            {"active_requests", resident == residency.end() ? 0 : resident->second.active_requests},
            {"has_vision", info->has_vision},
            {"reasoning", {
                {"supported", info->reasoning.supported},
                {"efforts", info->reasoning.efforts},
                {"default", info->reasoning.default_effort},
                {"none_disables", info->reasoning.none_disables},
                {"aliases", info->reasoning.aliases},
            }},
            {"alias", true},
            {"alias_target", alias.target},
            {"required_context_size", alias.required_context_size},
            {"required_capabilities", alias.required_capabilities},
        });
    }
    nlohmann::json running = nlohmann::json::array();
    for (const auto& resident : coordinator.residency()) {
        const auto& info = coordinator.registry().get_info(resident.name);
        running.push_back({
            {"id", resident.name},
            {"name", resident.name},
            {"loaded", true},
            {"primary", resident.primary},
            {"context_size", info.context_size},
            {"vram_required_mb", resident.estimated_vram_mb},
        });
    }
    return {{"models", models}, {"running", running}, {"current", loaded.value_or("")}};
}

nlohmann::json build_dashboard_jobs(const observability::StatsDb& stats_db,
                                    int limit = 100,
                                    const std::string& protocol_profile = {},
                                    const std::string& endpoint = {}) {
    nlohmann::json jobs = nlohmann::json::array();
    int index = 0;
    for (const auto& row : stats_db.recent_requests(
             limit, protocol_profile, endpoint)) {
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
            {"id", row.request_id.empty()
                ? "request-" + std::to_string(row.timestamp_unix_ms) + "-" + std::to_string(index++)
                : row.request_id},
            {"type", row.endpoint.empty() ? "legacy" : row.endpoint},
            {"status", row.status_code >= 200 && row.status_code < 300 ? "succeeded" : "failed"},
            {"model", row.model},
            {"resolvedModel", row.resolved_model},
            {"principalClass", row.principal_class},
            {"endpoint", row.endpoint},
            {"protocolProfile", row.protocol_profile},
            {"modality", row.modality},
            {"stream", row.stream},
            {"finishCode", row.finish_code},
            {"errorCode", row.error_code},
            {"createdAt", timestamp},
            {"timestampUnixMs", row.timestamp_unix_ms},
            {"promptTokens", row.prompt_tokens},
            {"cachedPromptTokens", row.cached_prompt_tokens},
            {"cacheWriteTokens", row.cache_write_tokens},
            {"completionTokens", row.completion_tokens},
            {"reasoningTokens", row.reasoning_tokens},
            {"totalTokens", row.prompt_tokens + row.completion_tokens},
            {"tokensPerSecond", row.tokens_per_second},
            {"promptTokensPerSecond", row.prompt_tokens_per_second},
            {"generationDurationMs", row.generation_duration_ms},
            {"promptDurationMs", row.prompt_duration_ms},
            {"queueDurationMs", row.queue_duration_ms},
            {"swapLoadDurationMs", row.swap_load_duration_ms},
            {"firstTokenDurationMs", row.first_token_duration_ms},
            {"durationMs", row.duration_ms},
            {"httpStatus", row.status_code},
            {"slotId", row.slot_id},
            {"inputAudioSeconds", row.input_audio_seconds},
            {"outputAudioSeconds", row.output_audio_seconds},
            {"inputCharacters", row.input_characters},
            {"inputImageCount", row.input_image_count},
            {"outputImageCount", row.output_image_count}
        });
    }
    return {{"jobs", jobs}};
}

nlohmann::json profile_candidate_json(
    const optimize::ProfileCandidate& candidate) {
    return {
        {"contextPerSlot", candidate.context_per_slot},
        {"slots", candidate.slots},
        {"nBatch", candidate.n_batch},
        {"nUbatch", candidate.n_ubatch},
        {"cacheTypeK", candidate.cache_type_k},
        {"cacheTypeV", candidate.cache_type_v},
        {"flashAttention", candidate.flash_attention},
        {"mtpMaxActiveRequests", candidate.mtp_max_active_requests},
        {"estimatedVramMb", candidate.estimated_vram_mb},
        {"reserveVramMb", candidate.reserve_vram_mb},
        {"qualityScore", candidate.quality_score},
        {"speedScore", candidate.speed_score},
        {"parallelismScore", candidate.parallelism_score},
        {"headroomScore", candidate.headroom_score},
        {"overallScore", candidate.overall_score},
        {"fits", candidate.fits},
        {"reasons", candidate.reasons},
    };
}

nlohmann::json profile_benchmark_snapshot_json(
    const ProfileBenchmarkSnapshot& snapshot) {
    const auto trial_json = [](const ProfileBenchmarkTrial& trial) {
        auto value = profile_candidate_json(trial.candidate);
        value["completed"] = trial.completed;
        value["error"] = trial.error;
        value["loadMs"] = trial.metrics.load_ms;
        value["promptTokensPerSecond"] = trial.metrics.prompt_tokens_per_second;
        value["averageTokensPerSecond"] = trial.metrics.average_tokens_per_second;
        value["parallelTokensPerSecond"] = trial.metrics.parallel_tokens_per_second;
        value["averageTimeToFirstTokenMs"] = trial.metrics.average_time_to_first_token_ms;
        value["peakVramMb"] = trial.metrics.peak_vram_mb;
        value["qualityPasses"] = trial.metrics.quality_passes;
        value["qualityTotal"] = trial.metrics.quality_total;
        value["performanceIndex"] = trial.metrics.performance_index;
        value["promptTokens"] = trial.metrics.prompt_tokens;
        value["completionTokens"] = trial.metrics.completion_tokens;
        value["concurrency"] = nlohmann::json::array();
        for (const auto& measured : trial.metrics.concurrency) {
            value["concurrency"].push_back({
                {"requests", measured.requests},
                {"aggregateTokensPerSecond",
                 measured.aggregate_tokens_per_second},
                {"averageRequestTokensPerSecond",
                 measured.average_request_tokens_per_second},
                {"mtpRequests", measured.mtp_requests},
                {"mtpDraftedTokens", measured.mtp_drafted_tokens},
                {"mtpAcceptedTokens", measured.mtp_accepted_tokens},
            });
        }
        value["outputSamples"] = trial.metrics.output_samples;
        return value;
    };
    nlohmann::json trials = nlohmann::json::array();
    for (const auto& trial : snapshot.trials) {
        trials.push_back(trial_json(trial));
    }
    nlohmann::json result = {
        {"id", snapshot.id},
        {"state", snapshot.state},
        {"stage", snapshot.stage},
        {"message", snapshot.message},
        {"model", snapshot.model},
        {"completedCandidates", snapshot.completed_candidates},
        {"totalCandidates", snapshot.total_candidates},
        {"progressPct", snapshot.progress_pct},
        {"startedUnixMs", snapshot.started_unix_ms},
        {"finishedUnixMs", snapshot.finished_unix_ms},
        {"measured", snapshot.measured},
        {"cancelRequested", snapshot.cancel_requested},
        {"restored", snapshot.restored},
        {"baseline", snapshot.has_baseline
            ? trial_json(snapshot.baseline) : nlohmann::json(nullptr)},
        {"weights", {
            {"promptProcessing", 0.50},
            {"generation", 0.50},
        }},
        {"candidates", std::move(trials)},
    };
    result["recommended"] = snapshot.has_recommendation
        ? profile_candidate_json(snapshot.recommended)
        : nlohmann::json(nullptr);
    return result;
}

double artifact_size_mb(const std::string& path) {
    if (path.empty()) return 0.0;
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    return error ? 0.0
                 : static_cast<double>(bytes) / (1024.0 * 1024.0);
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

nlohmann::json usage_bucket_json(
    const std::vector<observability::UsageBucketRow>& rows) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& row : rows) {
        out.push_back({
            {"bucket", row.bucket},
            {"model", row.model},
            {"promptTokens", row.prompt_tokens},
            {"cachedPromptTokens", row.cached_prompt_tokens},
            {"completionTokens", row.completion_tokens},
            {"measuredCompletionTokens", row.measured_completion_tokens},
            {"measuredPromptTokens", row.measured_prompt_tokens},
            {"totalTokens", row.total_tokens},
            {"requests", row.requests},
            {"successfulRequests", row.successful_requests},
            {"generationDurationMs", row.generation_duration_ms},
            {"promptDurationMs", row.prompt_duration_ms},
            {"peakTokensPerSecond", row.peak_tokens_per_second},
            {"peakPromptTokensPerSecond", row.peak_prompt_tokens_per_second},
            {"inputAudioSeconds", row.input_audio_seconds},
            {"inputCharacters", row.input_characters}
        });
    }
    return out;
}

nlohmann::json build_dashboard_status(const DashboardDeps& deps) {
    auto& coordinator = deps.gw.coordinator;
    const auto& metrics = *deps.gw.metrics;
    const auto& stats_db = *deps.gw.stats_db;

    auto hardware = gpu_hardware_json(deps.gpu.latest());
    auto system = system_hardware_json();
    for (auto it = system.begin(); it != system.end(); ++it) hardware[it.key()] = it.value();

    nlohmann::json usage = nlohmann::json::array();
    std::unordered_map<std::string, observability::UsageBucketRow> canonical_usage;
    for (const auto& bucket : stats_db.daily_usage(0)) {
        auto& total = canonical_usage[bucket.model];
        total.model = bucket.model;
        total.prompt_tokens += bucket.prompt_tokens;
        total.cached_prompt_tokens += bucket.cached_prompt_tokens;
        total.completion_tokens += bucket.completion_tokens;
        total.total_tokens += bucket.total_tokens;
        total.requests += bucket.requests;
        total.successful_requests += bucket.successful_requests;
        total.measured_completion_tokens += bucket.measured_completion_tokens;
        total.measured_prompt_tokens += bucket.measured_prompt_tokens;
        total.generation_duration_ms += bucket.generation_duration_ms;
        total.prompt_duration_ms += bucket.prompt_duration_ms;
        total.peak_tokens_per_second = std::max(
            total.peak_tokens_per_second, bucket.peak_tokens_per_second);
        total.peak_prompt_tokens_per_second = std::max(
            total.peak_prompt_tokens_per_second,
            bucket.peak_prompt_tokens_per_second);
        total.input_audio_seconds += bucket.input_audio_seconds;
        total.input_characters += bucket.input_characters;
    }
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t requests = 0;
    for (const auto& row : stats_db.model_usage()) {
        const auto& canonical = canonical_usage.at(row.model);
        prompt_tokens += canonical.prompt_tokens;
        completion_tokens += canonical.completion_tokens;
        requests += canonical.requests;
        const double avg_tps = canonical.generation_duration_ms > 0.0
            ? static_cast<double>(canonical.measured_completion_tokens) / (canonical.generation_duration_ms / 1000.0)
            : 0.0;
        const double avg_prompt_tps = canonical.prompt_duration_ms > 0.0
            ? static_cast<double>(canonical.measured_prompt_tokens) /
                (canonical.prompt_duration_ms / 1000.0)
            : 0.0;
        usage.push_back({
            {"model", row.model},
            {"requests", canonical.requests},
            {"successfulRequests", canonical.successful_requests},
            {"promptTokens", canonical.prompt_tokens},
            {"cachedPromptTokens", canonical.cached_prompt_tokens},
            {"completionTokens", canonical.completion_tokens},
            {"measuredCompletionTokens", canonical.measured_completion_tokens},
            {"measuredPromptTokens", canonical.measured_prompt_tokens},
            {"totalTokens", canonical.total_tokens},
            {"peakTokensPerSecond", canonical.peak_tokens_per_second},
            {"avgTokensPerSecond", avg_tps},
            {"peakPromptTokensPerSecond", canonical.peak_prompt_tokens_per_second},
            {"avgPromptTokensPerSecond", avg_prompt_tps},
            {"lastTimestampUnixMs", row.last_timestamp_unix_ms},
            {"inputAudioSeconds", canonical.input_audio_seconds},
            {"inputCharacters", canonical.input_characters}
        });
    }

    const auto monthly_rows = stats_db.monthly_usage();
    auto monthly = usage_bucket_json(monthly_rows);
    auto daily = usage_bucket_json(stats_db.daily_usage(31));
    auto hourly = usage_bucket_json(stats_db.hourly_usage(24));

    std::vector<double> latencies;
    for (const auto& row : stats_db.recent_requests(500)) {
        if (row.status_code >= 200 && row.status_code < 300 && row.duration_ms > 0.0) {
            latencies.push_back(row.duration_ms);
        }
    }
    std::sort(latencies.begin(), latencies.end());

    const auto swap = deps.gw.swap_tracker ? deps.gw.swap_tracker->snapshot() : SwapSnapshot{};
    bool gpu_locked = false;
    std::string gpu_lock_owner;
    for (const auto& item : coordinator.residency()) {
        if (item.active_requests <= 0 || item.estimated_vram_mb <= 0) continue;
        gpu_locked = true;
        if (gpu_lock_owner.empty() || item.primary) gpu_lock_owner = item.name;
    }
    auto model_json = build_dashboard_models(coordinator);
    nlohmann::json queued_requests = nlohmann::json::array();
    for (const auto& item : coordinator.queue()) {
        queued_requests.push_back({
            {"id", item.id},
            {"model", item.model},
            {"priority", item.priority},
            {"position", item.position},
            {"queuedMs", item.queued_ms},
            {"remainingMs", item.remaining_ms},
        });
    }
    return {
        {"status", "ok"},
        {"queue", {
            {"running", coordinator.active_request_count()},
            {"queued", coordinator.queued_request_count()},
            {"requests", queued_requests},
            {"gpuLocked", gpu_locked},
            {"lockOwner", gpu_lock_owner},
            {"vramBudgetMb", coordinator.vram_budget_mb()},
            {"vramAvailableMb", coordinator.vram_available_mb()},
            {"resourceDecision", coordinator.last_resource_decision()},
        }},
        {"swap", {
            {"swapping", swap.swapping},
            {"target", swap.target},
            {"from", swap.from},
            {"startedUnixMs", swap.started_unix_ms},
            {"lastError", swap.last_deferred ? std::string{} : swap.last_error}
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
        {"dailyTokenUsageAllTime", false},
        {"hourlyTokenUsage", hourly},
        {"models", model_json["models"]},
        {"current", model_json["current"]},
        {"uptime", deps.uptime_seconds ? deps.uptime_seconds() : 0}
    };
}

} // namespace

void register_dashboard_routes(httplib::Server& server, const DashboardDeps& deps,
                               const RouteWrapper& wrap) {
#include "dashboard_optimize_routes.ipp"

#include "dashboard_model_store_routes.ipp"
#include "dashboard_alias_routes.ipp"
#include "dashboard_config_routes.ipp"
#include "dashboard_status_routes.ipp"
#include "dashboard_observability_routes.ipp"
}

} // namespace inferdeck::gateway
