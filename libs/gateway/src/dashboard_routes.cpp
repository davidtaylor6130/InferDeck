#include "gateway/dashboard_routes.hpp"

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
#include <filesystem>
#include <fstream>
#include <iomanip>
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
std::mutex config_write_mutex;

foundation::Result<void> write_config_atomic(const std::filesystem::path& path,
                                             const std::string& text);
std::string read_text(const std::filesystem::path& path);

nlohmann::json alias_json(const model::ModelAlias& alias) {
    return {
        {"name", alias.name},
        {"target", alias.target},
        {"requiredContextSize", alias.required_context_size},
        {"requiredCapabilities", alias.required_capabilities},
    };
}

std::string replace_top_level_yaml_section(
    std::string text, const std::string& key, const std::string& value) {
    const std::string marker = key + ":";
    std::size_t start = std::string::npos;
    std::size_t end = text.size();
    std::size_t line_start = 0;
    while (line_start < text.size()) {
        const auto line_end = text.find('\n', line_start);
        const auto length = (line_end == std::string::npos ? text.size() : line_end) - line_start;
        const auto line = text.substr(line_start, length);
        if (start == std::string::npos && line.starts_with(marker)) {
            start = line_start;
        } else if (start != std::string::npos && !line.empty() &&
                   line.front() != ' ' && line.front() != '\t' &&
                   line.front() != '\r') {
            end = line_start;
            break;
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 1;
    }
    std::string block = marker;
    if (value == "[]") {
        block += " []\n";
    } else {
        block += "\n";
        std::size_t value_start = 0;
        while (value_start <= value.size()) {
            const auto value_end = value.find('\n', value_start);
            block += "  " + value.substr(
                value_start, value_end == std::string::npos
                    ? std::string::npos : value_end - value_start) + "\n";
            if (value_end == std::string::npos) break;
            value_start = value_end + 1;
        }
    }
    if (start == std::string::npos) {
        if (!text.empty() && text.back() != '\n') text += '\n';
        if (!text.empty()) text += '\n';
        text += block;
        return text;
    }
    text.replace(start, end - start, block);
    return text;
}

foundation::Result<std::string> remove_model_registry_entry(
    const std::string& text, const std::string& name) {
    const auto root = YAML::Load(text);
    const auto models = root["model_registry"];
    if (!models || !models.IsSequence()) {
        return foundation::Err<std::string>(
            foundation::ErrorCode::NotFound, "model registry is missing");
    }
    bool configured = false;
    for (const auto& entry : models) {
        if (entry["name"] && entry["name"].as<std::string>() == name) {
            configured = true;
            break;
        }
    }
    if (!configured) {
        return foundation::Err<std::string>(
            foundation::ErrorCode::NotFound, "configured model not found: " + name);
    }

    std::size_t section_start = std::string::npos;
    std::size_t section_end = text.size();
    std::vector<std::size_t> entries;
    std::size_t line_start = 0;
    while (line_start < text.size()) {
        const auto newline = text.find('\n', line_start);
        const auto line_end = newline == std::string::npos ? text.size() : newline;
        const auto line = std::string_view{text}.substr(line_start, line_end - line_start);
        if (section_start == std::string::npos) {
            if (line.starts_with("model_registry:")) section_start = line_start;
        } else {
            if (!line.empty() && line.front() != ' ' && line.front() != '\t' &&
                line.front() != '\r' && line.front() != '#') {
                section_end = line_start;
                break;
            }
            if (line.starts_with("  - ") || line == "  -") entries.push_back(line_start);
        }
        if (newline == std::string::npos) break;
        line_start = newline + 1;
    }
    if (section_start == std::string::npos) {
        return foundation::Err<std::string>(
            foundation::ErrorCode::NotFound, "model registry is missing");
    }

    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto begin = entries[index];
        const auto end = index + 1 < entries.size() ? entries[index + 1] : section_end;
        const auto block = text.substr(begin, end - begin);
        try {
            const auto parsed = YAML::Load("model_registry:\n" + block);
            const auto entry = parsed["model_registry"];
            if (entry && entry.IsSequence() && entry.size() == 1 &&
                entry[0]["name"] && entry[0]["name"].as<std::string>() == name) {
                auto removal_end = end;
                auto comment_start = begin;
                while (comment_start < end) {
                    const auto newline = text.find('\n', comment_start);
                    const auto line_end = newline == std::string::npos ? end : newline;
                    const auto line = std::string_view{text}.substr(
                        comment_start, line_end - comment_start);
                    if (comment_start > begin &&
                        (line.starts_with("#") || line.starts_with("  #"))) {
                        removal_end = comment_start;
                        break;
                    }
                    if (newline == std::string::npos || newline >= end) break;
                    comment_start = newline + 1;
                }
                std::string updated = text;
                updated.erase(begin, removal_end - begin);
                return foundation::Ok(std::move(updated));
            }
        } catch (const std::exception&) {
        }
    }
    return foundation::Err<std::string>(
        foundation::ErrorCode::ParseError,
        "could not locate the configured model block without rewriting the configuration");
}

foundation::Result<void> persist_aliases(
    const DashboardDeps& deps, const std::vector<model::ModelAlias>& aliases) {
    const auto source_path = !deps.active_config_file.empty() &&
            std::filesystem::exists(deps.active_config_file)
        ? deps.active_config_file : deps.base_config_file;
    const auto destination_path = deps.active_config_file.empty()
        ? deps.base_config_file : deps.active_config_file;
    const auto text = read_text(source_path);
    if (text.empty()) {
        return foundation::Err<void>(foundation::ErrorCode::IoError,
                                     "configuration file is unreadable");
    }
    try {
        YAML::Load(text);
        YAML::Node entries(YAML::NodeType::Sequence);
        for (const auto& alias : aliases) {
            YAML::Node entry;
            entry["name"] = alias.name;
            entry["target"] = alias.target;
            entry["required_context_size"] = alias.required_context_size;
            for (const auto& capability : alias.required_capabilities) {
                entry["required_capabilities"].push_back(capability);
            }
            entries.push_back(entry);
        }
        YAML::Emitter emitter;
        emitter << entries;
        if (!emitter.good()) {
            return foundation::Err<void>(foundation::ErrorCode::ParseError,
                                         emitter.GetLastError());
        }
        const std::string updated = replace_top_level_yaml_section(
            text, "model_aliases", emitter.c_str());
        if (deps.validate_config) {
            const auto valid = deps.validate_config(updated);
            if (!valid) return valid;
        }
        return write_config_atomic(destination_path, updated);
    } catch (const std::exception& error) {
        return foundation::Err<void>(foundation::ErrorCode::ParseError, error.what());
    }
}

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

struct ScalarSpan {
    std::size_t begin{0};
    std::size_t end{0};
};

std::optional<ScalarSpan> yaml_scalar_span(const std::string& text,
                                           const std::string& section,
                                           const std::string& key) {
    bool in_section = false;
    std::size_t position = 0;
    while (position < text.size()) {
        const auto newline = text.find('\n', position);
        const auto line_end = newline == std::string::npos ? text.size() : newline;
        std::string_view line(text.data() + position, line_end - position);
        const auto first = line.find_first_not_of(" \t\r");
        if (first != std::string_view::npos && line[first] != '#') {
            const auto colon = line.find(':', first);
            if (colon != std::string_view::npos) {
                const std::string name(line.substr(first, colon - first));
                if (first == 0) {
                    in_section = name == section;
                } else if (in_section && name == key) {
                    auto value_begin = colon + 1;
                    while (value_begin < line.size() &&
                           (line[value_begin] == ' ' || line[value_begin] == '\t')) ++value_begin;
                    auto value_end = line.size();
                    bool single = false;
                    bool double_quote = false;
                    for (std::size_t i = value_begin; i < line.size(); ++i) {
                        if (line[i] == '\'' && !double_quote) single = !single;
                        if (line[i] == '"' && !single && (i == 0 || line[i - 1] != '\\')) {
                            double_quote = !double_quote;
                        }
                        if (line[i] == '#' && !single && !double_quote &&
                            (i == value_begin || line[i - 1] == ' ' || line[i - 1] == '\t')) {
                            value_end = i;
                            break;
                        }
                    }
                    while (value_end > value_begin &&
                           (line[value_end - 1] == ' ' || line[value_end - 1] == '\t' ||
                            line[value_end - 1] == '\r')) --value_end;
                    return ScalarSpan{position + value_begin, position + value_end};
                }
            }
        }
        if (newline == std::string::npos) break;
        position = newline + 1;
    }
    return std::nullopt;
}

bool secret_field(std::string_view section, std::string_view key) {
    return (section == "auth" && key == "token") ||
        (section == "control" && key == "token") ||
        (section == "model_store" && key == "hf_token");
}

bool secret_nodes_are_masked(const std::string& text) {
    try {
        const auto root = YAML::Load(text);
        if (!root || !root.IsMap()) return true;
        for (const auto& section : root) {
            if (!section.first.IsScalar() || !section.second.IsMap()) continue;
            const auto section_name = section.first.as<std::string>();
            for (const auto& field : section.second) {
                if (!field.first.IsScalar()) continue;
                const auto key = field.first.as<std::string>();
                if (!secret_field(section_name, key)) continue;
                if (!field.second.IsScalar() ||
                    field.second.as<std::string>() != "__INFERDECK_SECRET__") {
                    return false;
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string serialize_masked_config(const std::string& text) {
    try {
        auto root = YAML::Load(text);
        if (!root || !root.IsMap()) return {};
        for (auto section : root) {
            if (!section.first.IsScalar() || !section.second.IsMap()) continue;
            const auto section_name = section.first.as<std::string>();
            for (auto field : section.second) {
                if (!field.first.IsScalar()) continue;
                const auto key = field.first.as<std::string>();
                if (secret_field(section_name, key)) {
                    field.second = "__INFERDECK_SECRET__";
                }
            }
        }
        return YAML::Dump(root);
    } catch (...) {
        return {};
    }
}

std::string mask_config_secret(std::string text) {
    if (auto span = yaml_scalar_span(text, "auth", "token")) {
        text.replace(span->begin, span->end - span->begin, "\"__INFERDECK_SECRET__\"");
    }
    if (auto span = yaml_scalar_span(text, "control", "token")) {
        text.replace(span->begin, span->end - span->begin, "\"__INFERDECK_SECRET__\"");
    }
    if (auto span = yaml_scalar_span(text, "model_store", "hf_token")) {
        text.replace(span->begin, span->end - span->begin, "\"__INFERDECK_SECRET__\"");
    }
    if (secret_nodes_are_masked(text)) return text;
    return serialize_masked_config(text);
}

std::string restore_config_secret(std::string submitted, const std::string& current) {
    const auto original_submitted = submitted;
    for (const auto& [section, key] : std::array{
             std::pair{"auth", "token"}, std::pair{"control", "token"},
             std::pair{"model_store", "hf_token"}}) {
        const auto submitted_span = yaml_scalar_span(submitted, section, key);
        const auto current_span = yaml_scalar_span(current, section, key);
        if (!submitted_span || !current_span) continue;
        std::string value = submitted.substr(submitted_span->begin,
                                             submitted_span->end - submitted_span->begin);
        if (value == "__INFERDECK_SECRET__" || value == "\"__INFERDECK_SECRET__\"" ||
            value == "'__INFERDECK_SECRET__'") {
            submitted.replace(submitted_span->begin, submitted_span->end - submitted_span->begin,
                              current.substr(current_span->begin, current_span->end - current_span->begin));
        }
    }
    if (submitted.find("__INFERDECK_SECRET__") == std::string::npos) return submitted;
    try {
        auto submitted_root = YAML::Load(original_submitted);
        const auto current_root = YAML::Load(current);
        if (!submitted_root || !submitted_root.IsMap() ||
            !current_root || !current_root.IsMap()) {
            return submitted;
        }
        for (auto section : submitted_root) {
            if (!section.first.IsScalar() || !section.second.IsMap()) continue;
            const auto section_name = section.first.as<std::string>();
            for (auto field : section.second) {
                if (!field.first.IsScalar() || !field.second.IsScalar()) continue;
                const auto key = field.first.as<std::string>();
                if (!secret_field(section_name, key) ||
                    field.second.as<std::string>() != "__INFERDECK_SECRET__") {
                    continue;
                }
                const auto replacement = current_root[section_name][key];
                if (replacement) field.second = replacement.as<std::string>();
            }
        }
        return YAML::Dump(submitted_root);
    } catch (...) {
        return submitted;
    }
}

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

foundation::Result<void> write_config_atomic(const std::filesystem::path& path,
                                              const std::string& text) {
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return foundation::Err<void>(foundation::ErrorCode::IoError,
                                         "cannot open temporary configuration file");
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            return foundation::Err<void>(foundation::ErrorCode::IoError,
                                         "cannot write temporary configuration file");
        }
    }
#ifdef _WIN32
    if (!MoveFileExA(temporary.c_str(), path.string().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        return foundation::Err<void>(foundation::ErrorCode::IoError,
                                     "cannot replace configuration file");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        return foundation::Err<void>(foundation::ErrorCode::IoError, error.message());
    }
#endif
    return foundation::Ok();
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
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
            {"modality", info.modality},
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
            {"has_vision", info.has_vision},
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
            {"resolvedModel", row.resolved_model},
            {"createdAt", timestamp},
            {"timestampUnixMs", row.timestamp_unix_ms},
            {"promptTokens", row.prompt_tokens},
            {"cachedPromptTokens", row.cached_prompt_tokens},
            {"completionTokens", row.completion_tokens},
            {"totalTokens", row.prompt_tokens + row.completion_tokens},
            {"tokensPerSecond", row.tokens_per_second},
            {"promptTokensPerSecond", row.prompt_tokens_per_second},
            {"generationDurationMs", row.generation_duration_ms},
            {"promptDurationMs", row.prompt_duration_ms},
            {"durationMs", row.duration_ms},
            {"httpStatus", row.status_code},
            {"slotId", row.slot_id},
            {"inputAudioSeconds", row.input_audio_seconds},
            {"inputCharacters", row.input_characters}
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
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t requests = 0;
    for (const auto& row : stats_db.model_usage()) {
        prompt_tokens += row.prompt_tokens;
        completion_tokens += row.completion_tokens;
        requests += row.requests;
        const double avg_tps = row.total_generation_duration_ms > 0.0
            ? static_cast<double>(row.measured_completion_tokens) / (row.total_generation_duration_ms / 1000.0)
            : 0.0;
        const double avg_prompt_tps = row.total_prompt_duration_ms > 0.0
            ? static_cast<double>(row.measured_prompt_tokens) /
                (row.total_prompt_duration_ms / 1000.0)
            : 0.0;
        usage.push_back({
            {"model", row.model},
            {"requests", row.requests},
            {"successfulRequests", row.successful_requests},
            {"promptTokens", row.prompt_tokens},
            {"cachedPromptTokens", row.cached_prompt_tokens},
            {"completionTokens", row.completion_tokens},
            {"measuredCompletionTokens", row.measured_completion_tokens},
            {"measuredPromptTokens", row.measured_prompt_tokens},
            {"totalTokens", row.prompt_tokens + row.completion_tokens},
            {"peakTokensPerSecond", row.peak_tokens_per_second},
            {"avgTokensPerSecond", avg_tps},
            {"peakPromptTokensPerSecond", row.peak_prompt_tokens_per_second},
            {"avgPromptTokensPerSecond", avg_prompt_tps},
            {"lastTimestampUnixMs", row.last_timestamp_unix_ms},
            {"inputAudioSeconds", row.input_audio_seconds},
            {"inputCharacters", row.input_characters}
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
    server.Post(R"(^/api/optimize/profile$)",
                wrap([deps](const httplib::Request& req,
                            httplib::Response& resp) {
        if (deps.gw.coordinator.active_request_count() > 0 ||
            deps.gw.coordinator.queued_request_count() > 0) {
            write_error(resp, 409, "optimization_busy",
                        "profile analysis waits until active and queued requests are zero");
            return;
        }
        if (deps.gw.swap_tracker && deps.gw.swap_tracker->snapshot().swapping) {
            write_error(resp, 409, "optimization_busy",
                        "profile analysis waits until the current model swap finishes");
            return;
        }
        const auto gpu = deps.gpu.latest();
        if (!gpu.available || gpu.vram_total_mb <= 0.0) {
            write_error(resp, 503, "optimization_hardware_unavailable",
                        "GPU VRAM telemetry is required for profile analysis");
            return;
        }
        if (gpu.utilization_pct > 20.0) {
            write_error(resp, 409, "optimization_busy",
                        "GPU utilization is above the 20 percent safety threshold");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            const std::string model_name = body.value("model", "");
            auto registered =
                deps.gw.coordinator.registry().get_info_result(model_name);
            if (!registered) {
                write_error(resp, 404, "optimization_model_not_found",
                            registered.error().message);
                return;
            }
            const auto& info = *registered;
            if (info.runtime != "llama_cpp") {
                write_error(resp, 400, "optimization_runtime_unsupported",
                            "profile analysis currently supports llama.cpp LLM models");
                return;
            }

            optimize::ProfileInput input;
            input.model = model_name;
            input.total_vram_mb = gpu.vram_total_mb;
            input.model_file_mb =
                artifact_size_mb(info.gguf_path) +
                (info.has_vision ? artifact_size_mb(info.mmproj_path) : 0.0);
            input.configured_vram_mb = info.vram_required_mb;
            input.context_per_slot =
                body.value("contextPerSlot", info.context_size);
            input.slots = body.value("slots", info.n_slots);
            input.min_slots = body.value("minSlots", info.min_slots);
            input.n_batch = body.value("nBatch", 512);
            input.n_ubatch = body.value("nUbatch", input.n_batch);
            input.cache_type_k =
                body.value("cacheTypeK", std::string{"q8_0"});
            input.cache_type_v =
                body.value("cacheTypeV", std::string{"q8_0"});
            input.flash_attention =
                body.value("flashAttention", std::string{"auto"});

            if (deps.gw.stats_db) {
                for (const auto& usage : deps.gw.stats_db->model_usage()) {
                    if (usage.model != model_name ||
                        usage.total_duration_ms <= 0.0) {
                        continue;
                    }
                    input.observed_tokens_per_second =
                        static_cast<double>(usage.completion_tokens) /
                        (usage.total_duration_ms / 1000.0);
                    break;
                }
            }

            const auto result = optimize::recommend_profile(input);
            nlohmann::json candidates = nlohmann::json::array();
            for (const auto& candidate : result.candidates) {
                candidates.push_back(profile_candidate_json(candidate));
            }
            write_json(resp, 200, {
                {"model", model_name},
                {"mode", "profile_estimate"},
                {"measured", result.measured},
                {"observedTokensPerSecond",
                 input.observed_tokens_per_second},
                {"modelFileMb", input.model_file_mb},
                {"totalVramMb", input.total_vram_mb},
                {"weights", {
                    {"quality", result.quality_weight},
                    {"speed", result.speed_weight},
                    {"parallelism", result.parallelism_weight},
                    {"headroom", result.headroom_weight},
                }},
                {"recommended",
                 profile_candidate_json(result.recommended)},
                {"candidates", std::move(candidates)},
                {"notes", result.notes},
            });
        } catch (const std::invalid_argument& error) {
            write_error(resp, 400, "invalid_optimization_profile",
                        error.what());
        } catch (const std::exception& error) {
            write_error(resp, 422, "optimization_failed", error.what());
        }
    }));

    server.Post(R"(^/api/optimize/benchmark$)",
                wrap([deps](const httplib::Request& req,
                            httplib::Response& resp) {
        if (!deps.profile_benchmark) {
            write_error(resp, 503, "benchmark_unavailable",
                        "measured profile benchmark is unavailable");
            return;
        }
        const auto gpu = deps.gpu.latest();
        if (!gpu.available || gpu.vram_total_mb <= 0.0) {
            write_error(resp, 503, "optimization_hardware_unavailable",
                        "GPU VRAM telemetry is required for measured benchmarking");
            return;
        }
        if (gpu.utilization_pct > 20.0) {
            write_error(resp, 409, "optimization_busy",
                        "GPU utilization is above the 20 percent safety threshold");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            const std::string model_name = body.value("model", "");
            auto registered =
                deps.gw.coordinator.registry().get_info_result(model_name);
            if (!registered) {
                write_error(resp, 404, "optimization_model_not_found",
                            registered.error().message);
                return;
            }
            const auto& info = *registered;
            if (info.runtime != "llama_cpp") {
                write_error(resp, 400, "optimization_runtime_unsupported",
                            "measured benchmarking currently supports llama.cpp LLM models");
                return;
            }
            optimize::ProfileInput input;
            input.model = model_name;
            input.total_vram_mb = gpu.vram_total_mb;
            input.model_file_mb =
                artifact_size_mb(info.gguf_path) +
                (info.has_vision ? artifact_size_mb(info.mmproj_path) : 0.0);
            input.configured_vram_mb = info.vram_required_mb;
            input.context_per_slot =
                body.value("contextPerSlot", info.context_size);
            input.slots = body.value("slots", info.n_slots);
            input.min_slots = body.value("minSlots", info.min_slots);
            input.n_batch = body.value("nBatch", 512);
            input.n_ubatch = body.value("nUbatch", input.n_batch);
            input.cache_type_k =
                body.value("cacheTypeK", std::string{"q8_0"});
            input.cache_type_v =
                body.value("cacheTypeV", std::string{"q8_0"});
            const auto started = deps.profile_benchmark->start(
                info, input, body.value("candidateLimit", 3));
            if (!started) {
                const int status =
                    started.error().code == foundation::ErrorCode::InvalidArgument
                        ? 400 : 409;
                write_error(resp, status, "benchmark_not_started",
                            started.error().message);
                return;
            }
            write_json(resp, 202,
                       profile_benchmark_snapshot_json(*started));
        } catch (const nlohmann::json::exception& error) {
            write_error(resp, 400, "invalid_benchmark_request", error.what());
        } catch (const std::exception& error) {
            write_error(resp, 422, "benchmark_not_started", error.what());
        }
    }));

    server.Get(R"(^/api/optimize/benchmark$)",
               wrap([deps](const httplib::Request&,
                           httplib::Response& resp) {
        if (!deps.profile_benchmark) {
            write_error(resp, 503, "benchmark_unavailable",
                        "measured profile benchmark is unavailable");
            return;
        }
        write_json(resp, 200, profile_benchmark_snapshot_json(
            deps.profile_benchmark->snapshot()));
    }));

    server.Get(R"(^/api/optimize/schedule$)",
               wrap([deps](const httplib::Request&, httplib::Response& resp) {
        if (!deps.profile_benchmark_scheduler) {
            write_error(resp, 503, "optimization_schedule_unavailable",
                        "scheduled optimization is unavailable");
            return;
        }
        nlohmann::json schedules = nlohmann::json::array();
        for (const auto& status : deps.profile_benchmark_scheduler->statuses()) {
            schedules.push_back({
                {"model", status.model},
                {"enabled", status.enabled},
                {"windowStart", status.window_start},
                {"windowEnd", status.window_end},
                {"nextRunUnixMs", status.next_run_unix_ms},
                {"lastStartedUnixMs", status.last_started_unix_ms},
                {"lastFinishedUnixMs", status.last_finished_unix_ms},
                {"lastOutcome", status.last_outcome},
                {"lastMessage", status.last_message},
            });
        }
        write_json(resp, 200, {
            {"timezone", deps.profile_benchmark_scheduler->timezone_name()},
            {"schedules", std::move(schedules)},
        });
    }));

    server.Post(R"(^/api/optimize/benchmark/cancel$)",
                wrap([deps](const httplib::Request&,
                            httplib::Response& resp) {
        if (!deps.profile_benchmark) {
            write_error(resp, 503, "benchmark_unavailable",
                        "measured profile benchmark is unavailable");
            return;
        }
        const auto cancelled = deps.profile_benchmark->cancel();
        if (!cancelled) {
            write_error(resp, 409, "benchmark_not_running",
                        cancelled.error().message);
            return;
        }
        write_json(resp, 202, profile_benchmark_snapshot_json(
            deps.profile_benchmark->snapshot()));
    }));

    server.Get(R"(^/api/model-store/search$)", wrap([deps](const httplib::Request& req,
                                                            httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        const std::string query = req.has_param("q") ? req.get_param_value("q") : "";
        const std::string runtime = req.has_param("runtime") ? req.get_param_value("runtime") : "";
        const std::string modality = req.has_param("modality") ? req.get_param_value("modality") : "";
        int limit = 20;
        if (req.has_param("limit")) {
            try {
                limit = std::clamp(std::stoi(req.get_param_value("limit")), 1, 100);
            } catch (...) {
                write_error(resp, 400, "invalid_model_search", "limit must be an integer");
                return;
            }
        }
        auto result = deps.model_store->search(query, runtime, modality, limit);
        if (!result) {
            write_error(resp, result.error().code == foundation::ErrorCode::InvalidArgument ? 400 : 502,
                        "model_search_failed", result.error().message);
            return;
        }
        write_json(resp, 200, {{"models", std::move(*result)}});
    }));

    server.Get(R"(^/api/model-store/inspect$)", wrap([deps](const httplib::Request& req,
                                                             httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        const std::string repo = req.has_param("repo") ? req.get_param_value("repo") : "";
        auto result = deps.model_store->inspect(repo);
        if (!result) {
            const int status = result.error().code == foundation::ErrorCode::InvalidArgument ? 400 :
                               result.error().code == foundation::ErrorCode::NotFound ? 404 : 502;
            write_error(resp, status, "model_inspect_failed", result.error().message);
            return;
        }
        write_json(resp, 200, std::move(*result));
    }));

    server.Get(R"(^/api/model-store/downloads$)", wrap([deps](const httplib::Request&,
                                                               httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        nlohmann::json downloads = nlohmann::json::array();
        for (const auto& download : deps.model_store->downloads()) downloads.push_back(to_json(download));
        write_json(resp, 200, {{"downloads", std::move(downloads)},
                               {"installed", deps.model_store->installed()},
                               {"library", deps.model_store->library()}});
    }));

    server.Post(R"(^/api/model-store/downloads$)", wrap([deps](const httplib::Request& req,
                                                                httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            auto result = deps.model_store->install(
                body.value("repo", ""), body.value("filename", ""),
                body.value("runtime", ""), body.value("modality", ""),
                body.value("modelName", ""));
            if (!result) {
                const int status = result.error().code == foundation::ErrorCode::AlreadyExists ? 409 : 400;
                write_error(resp, status, "model_install_failed", result.error().message);
                return;
            }
            write_json(resp, 202, {{"id", *result}, {"state", "queued"}});
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_model_install", error.what());
        }
    }));

    server.Post(R"(^/api/model-store/downloads/([0-9]+)/(cancel|resume)$)",
                wrap([deps](const httplib::Request& req, httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        const auto id = static_cast<std::uint64_t>(std::stoull(req.matches[1].str()));
        auto result = req.matches[2].str() == "cancel" ? deps.model_store->cancel(id) :
                                                        deps.model_store->resume(id);
        if (!result) {
            write_error(resp, result.error().code == foundation::ErrorCode::NotFound ? 404 : 409,
                        "download_control_failed", result.error().message);
            return;
        }
        write_json(resp, 200, {{"ok", true}});
    }));

    server.Post(R"(^/api/model-store/(remove|archive)$)", wrap([deps](const httplib::Request& req,
                                                             httplib::Response& resp) {
        if (!deps.model_store) {
            write_error(resp, 503, "model_store_unavailable", "model store is unavailable");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            auto result = req.matches[1].str() == "archive"
                ? deps.model_store->archive(body.value("model", ""))
                : deps.model_store->remove(body.value("model", ""));
            if (!result) {
                const int status = result.error().code == foundation::ErrorCode::NotFound ? 404 : 409;
                write_error(resp, status, "model_retire_failed", result.error().message);
                return;
            }
            write_json(resp, 200, {{"ok", true}});
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_model_retire", error.what());
        }
    }));

    server.Post(R"(^/api/model-store/unregister$)", wrap([deps](const httplib::Request& req,
                                                                httplib::Response& resp) {
        std::lock_guard lock(config_write_mutex);
        try {
            const auto body = nlohmann::json::parse(req.body);
            const std::string name = body.value("model", "");
            if (name.empty()) {
                write_error(resp, 400, "missing_model", "request body must include model");
                return;
            }
            const auto info = deps.gw.coordinator.registry().get_info_result(name);
            if (!info) {
                write_error(resp, 404, "model_not_found", info.error().message);
                return;
            }
            const auto source_path = !deps.active_config_file.empty() &&
                    std::filesystem::exists(deps.active_config_file)
                ? deps.active_config_file : deps.base_config_file;
            const auto destination_path = deps.active_config_file.empty()
                ? deps.base_config_file : deps.active_config_file;
            const auto original = read_text(source_path);
            if (original.empty()) {
                write_error(resp, 500, "config_read_failed",
                            "configuration file is empty or unreadable");
                return;
            }
            const auto root = YAML::Load(original);
            if (root["default_model"] &&
                root["default_model"].as<std::string>() == name) {
                write_error(resp, 409, "default_model",
                            "change the default model before removing " + name);
                return;
            }
            for (const auto& alias : deps.gw.coordinator.registry().aliases()) {
                if (alias.target == name) {
                    write_error(resp, 409, "model_has_alias",
                                "remove or retarget alias " + alias.name + " first");
                    return;
                }
            }
            const auto updated = remove_model_registry_entry(original, name);
            if (!updated) {
                const int status = updated.error().code == foundation::ErrorCode::NotFound
                    ? 404 : 409;
                write_error(resp, status, "model_unregister_failed", updated.error().message);
                return;
            }
            if (deps.validate_config) {
                const auto valid = deps.validate_config(*updated);
                if (!valid) {
                    write_error(resp, 400, "invalid_configuration", valid.error().message);
                    return;
                }
            }
            const auto removed = deps.gw.coordinator.unregister(name);
            if (!removed) {
                write_error(resp, 409, "model_unregister_failed", removed.error().message);
                return;
            }
            const auto written = write_config_atomic(destination_path, *updated);
            if (!written) {
                deps.gw.coordinator.registry().register_model(*info);
                write_error(resp, 500, "model_unregister_persist_failed",
                            written.error().message);
                return;
            }
            write_json(resp, 200, {
                {"ok", true},
                {"filesDeleted", false},
                {"restartRequired", false},
            });
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_model_unregister", error.what());
        }
    }));

    server.Get(R"(^/api/model-aliases$)", wrap([deps](const httplib::Request&,
                                                       httplib::Response& resp) {
        nlohmann::json aliases = nlohmann::json::array();
        for (const auto& alias : deps.gw.coordinator.registry().aliases()) {
            aliases.push_back(alias_json(alias));
        }
        write_json(resp, 200, {{"aliases", std::move(aliases)}});
    }));

    server.Put(R"(^/api/model-aliases/([A-Za-z0-9_.-]+)$)",
               wrap([deps](const httplib::Request& req, httplib::Response& resp) {
        std::lock_guard lock(config_write_mutex);
        const std::string name = req.matches[1].str();
        if (name.empty() || name.size() > 128) {
            write_error(resp, 400, "invalid_model_alias", "alias name is invalid");
            return;
        }
        try {
            const auto body = nlohmann::json::parse(req.body);
            model::ModelAlias requested;
            requested.name = name;
            requested.target = body.value("target", "");
            requested.required_context_size = body.value("requiredContextSize", 0);
            if (body.contains("requiredCapabilities")) {
                requested.required_capabilities =
                    body["requiredCapabilities"].get<std::vector<std::string>>();
            }
            std::optional<model::ModelAlias> previous;
            for (const auto& alias : deps.gw.coordinator.registry().aliases()) {
                if (alias.name == name) previous = alias;
            }
            const auto applied = deps.gw.coordinator.registry().set_alias(std::move(requested));
            if (!applied) {
                write_error(resp, 409, "incompatible_model_alias", applied.error().message);
                return;
            }
            const auto persisted = persist_aliases(
                deps, deps.gw.coordinator.registry().aliases());
            if (!persisted) {
                if (previous) (void)deps.gw.coordinator.registry().set_alias(*previous);
                else (void)deps.gw.coordinator.registry().remove_alias(name);
                write_error(resp, 500, "model_alias_persist_failed", persisted.error().message);
                return;
            }
            write_json(resp, previous ? 200 : 201, alias_json(*applied));
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_model_alias", error.what());
        }
    }));

    server.Delete(R"(^/api/model-aliases/([A-Za-z0-9_.-]+)$)",
                  wrap([deps](const httplib::Request& req, httplib::Response& resp) {
        std::lock_guard lock(config_write_mutex);
        const std::string name = req.matches[1].str();
        std::optional<model::ModelAlias> previous;
        for (const auto& alias : deps.gw.coordinator.registry().aliases()) {
            if (alias.name == name) previous = alias;
        }
        if (!previous) {
            write_error(resp, 404, "model_alias_not_found", "model alias not found: " + name);
            return;
        }
        const auto removed = deps.gw.coordinator.registry().remove_alias(name);
        if (!removed) {
            write_error(resp, 404, "model_alias_not_found", removed.error().message);
            return;
        }
        const auto persisted = persist_aliases(
            deps, deps.gw.coordinator.registry().aliases());
        if (!persisted) {
            (void)deps.gw.coordinator.registry().set_alias(*previous);
            write_error(resp, 500, "model_alias_persist_failed", persisted.error().message);
            return;
        }
        write_json(resp, 200, {{"ok", true}});
    }));

    server.Get(R"(^/api/config$)", wrap([deps](const httplib::Request& req,
                                               httplib::Response& resp) {
        (void)req;
        if (deps.base_config_file.empty()) {
            write_error(resp, 503, "config_unavailable", "configuration path is unavailable");
            return;
        }
        const std::string base = read_text(deps.base_config_file);
        if (base.empty()) {
            write_error(resp, 500, "config_read_failed", "configuration file is empty or unreadable");
            return;
        }
        const std::string active = deps.active_config_file.empty()
            ? std::string{}
            : read_text(deps.active_config_file);
        const std::string desired_revision = config_revision(active.empty() ? base : active);
        write_json(resp, 200, {
            {"yaml", mask_config_secret(base)},
            {"revision", config_revision(base)},
            {"activeYaml", mask_config_secret(active.empty() ? base : active)},
            {"activeRevision", desired_revision},
            {"runningRevision", deps.running_config_revision},
            {"hasActiveProfile", !active.empty()},
            {"usingActiveProfile", deps.using_active_config},
            {"fallbackReason", deps.config_fallback_reason},
            {"restartRequired", deps.running_config_revision != desired_revision},
            {"secretSentinel", "__INFERDECK_SECRET__"},
        });
    }));

    server.Put(R"(^/api/config$)", wrap([deps](const httplib::Request& req,
                                               httplib::Response& resp) {
        if (maintenance_mode_active(deps.gw)) {
            write_error(resp, 503, "maintenance_mode",
                        "configuration cannot change during a measured benchmark");
            return;
        }
        static std::mutex write_mutex;
        std::lock_guard lock(write_mutex);
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_json", error.what());
            return;
        }
        if (!body.contains("yaml") || !body["yaml"].is_string() ||
            !body.contains("revision") || !body["revision"].is_string()) {
            write_error(resp, 400, "invalid_config_update", "yaml and revision are required");
            return;
        }
        std::string submitted = body["yaml"].get<std::string>();
        if (submitted.empty() || submitted.size() > 2 * 1024 * 1024) {
            write_error(resp, 400, "invalid_config_update", "configuration size is invalid");
            return;
        }
        const std::string current = read_text(deps.base_config_file);
        if (current.empty()) {
            write_error(resp, 500, "config_read_failed", "configuration file is empty or unreadable");
            return;
        }
        if (body["revision"].get<std::string>() != config_revision(current)) {
            write_error(resp, 409, "config_conflict", "configuration changed; reload before saving");
            return;
        }
        submitted = restore_config_secret(std::move(submitted), current);
        if (!deps.validate_config) {
            write_error(resp, 503, "config_validation_unavailable", "configuration validator is unavailable");
            return;
        }
        if (!deps.request_config_reload) {
            write_error(resp, 503, "config_reload_unavailable", "automatic configuration reload is unavailable");
            return;
        }
        auto valid = deps.validate_config(submitted);
        if (!valid) {
            write_error(resp, 400, "invalid_configuration", valid.error().message);
            return;
        }
        auto written = write_config_atomic(deps.base_config_file, submitted);
        if (!written) {
            write_error(resp, 500, "config_write_failed", written.error().message);
            return;
        }
        write_json(resp, 200, {
            {"ok", true},
            {"revision", config_revision(submitted)},
            {"restartRequired", true},
        });
    }));

    server.Put(R"(^/api/config/active$)", wrap([deps](const httplib::Request& req,
                                                      httplib::Response& resp) {
        if (maintenance_mode_active(deps.gw)) {
            write_error(resp, 503, "maintenance_mode",
                        "configuration cannot change during a measured benchmark");
            return;
        }
        static std::mutex active_write_mutex;
        std::lock_guard lock(active_write_mutex);
        if (deps.base_config_file.empty() || deps.active_config_file.empty()) {
            write_error(resp, 503, "config_unavailable", "configuration paths are unavailable");
            return;
        }
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& error) {
            write_error(resp, 400, "invalid_json", error.what());
            return;
        }
        if (!body.contains("yaml") || !body["yaml"].is_string() ||
            !body.contains("revision") || !body["revision"].is_string()) {
            write_error(resp, 400, "invalid_config_update", "yaml and revision are required");
            return;
        }
        std::string submitted = body["yaml"].get<std::string>();
        if (submitted.empty() || submitted.size() > 2 * 1024 * 1024) {
            write_error(resp, 400, "invalid_config_update", "configuration size is invalid");
            return;
        }
        const std::string base = read_text(deps.base_config_file);
        const std::string saved_active = read_text(deps.active_config_file);
        const std::string& current = saved_active.empty() ? base : saved_active;
        if (current.empty()) {
            write_error(resp, 500, "config_read_failed", "configuration file is empty or unreadable");
            return;
        }
        if (body["revision"].get<std::string>() != config_revision(current)) {
            write_error(resp, 409, "config_conflict", "active configuration changed; reload before saving");
            return;
        }
        submitted = restore_config_secret(std::move(submitted), current);
        if (!deps.validate_config) {
            write_error(resp, 503, "config_validation_unavailable", "configuration validator is unavailable");
            return;
        }
        auto valid = deps.validate_config(submitted);
        if (!valid) {
            write_error(resp, 400, "invalid_configuration", valid.error().message);
            return;
        }
        auto written = write_config_atomic(deps.active_config_file, submitted);
        if (!written) {
            write_error(resp, 500, "config_write_failed", written.error().message);
            return;
        }
        auto reload = deps.request_config_reload();
        if (!reload) {
            write_error(resp, 503, "config_reload_failed", reload.error().message);
            return;
        }
        write_json(resp, 200, {
            {"ok", true},
            {"activeRevision", config_revision(submitted)},
            {"hasActiveProfile", true},
            {"restartRequired", false},
            {"applyScheduled", true},
        });
    }));

    server.Delete(R"(^/api/config/active$)", wrap([deps](const httplib::Request&,
                                                         httplib::Response& resp) {
        if (maintenance_mode_active(deps.gw)) {
            write_error(resp, 503, "maintenance_mode",
                        "configuration cannot change during a measured benchmark");
            return;
        }
        if (deps.active_config_file.empty()) {
            write_error(resp, 503, "config_unavailable", "active configuration path is unavailable");
            return;
        }
        if (!deps.request_config_reload) {
            write_error(resp, 503, "config_reload_unavailable", "automatic configuration reload is unavailable");
            return;
        }
        std::error_code error;
        const bool removed = std::filesystem::remove(deps.active_config_file, error);
        if (error) {
            write_error(resp, 500, "config_reset_failed", error.message());
            return;
        }
        if (removed) {
            auto reload = deps.request_config_reload();
            if (!reload) {
                write_error(resp, 503, "config_reload_failed", reload.error().message);
                return;
            }
        }
        write_json(resp, 200, {
            {"ok", true},
            {"removed", removed},
            {"hasActiveProfile", false},
            {"restartRequired", false},
            {"applyScheduled", removed},
        });
    }));

    server.Get(R"(^/api/status$)", wrap([deps](const httplib::Request& req,
                                               httplib::Response& resp) {
        (void)req;
        resp.set_content(build_dashboard_status(deps).dump(), "application/json");
    }));

    server.Get(R"(^/api/inferdeck/v1/usage/daily$)",
               wrap([deps](const httplib::Request& req,
                           httplib::Response& resp) {
        (void)req;
        write_json(resp, 200, {
            {"dailyTokenUsage",
             usage_bucket_json(deps.gw.stats_db->daily_usage(0))},
            {"dailyTokenUsageAllTime", true},
        });
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
        const auto body = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
        const auto current = deps.gw.coordinator.get_loaded_model();
        const std::string requested_model = body.value("model", current.value_or(""));
        const auto resolved = requested_model.empty()
            ? foundation::Ok(std::string{})
            : deps.gw.coordinator.registry().resolve(requested_model);
        if (!resolved) {
            write_error(resp, 404, "model_not_found", resolved.error().message);
            return;
        }
        const std::string& model_name = *resolved;
        if (!model_name.empty() && maintenance_blocks_model(deps.gw, model_name)) {
            write_error(resp, 503, "maintenance_mode",
                        "measured model optimization is using the same compute resource");
            return;
        }
        auto result = model_name.empty() ? foundation::Ok() : deps.gw.coordinator.unload(model_name);
        if (!result) {
            write_error(resp, 500, "unload_failed", result.error().message);
            return;
        }
        if (deps.gw.events && !model_name.empty()) {
            deps.gw.events->publish("model", nlohmann::json{
                {"state", "unloaded"},
                {"from", model_name},
                {"to", ""},
                {"durationMs", 0.0},
                {"error", ""},
            }.dump());
        }
        write_json(resp, 200, {{"ok", true}, {"status", "stopped"},
                               {"model", requested_model},
                               {"resolvedModel", model_name}});
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
            } catch (const std::exception& error) {
                LOG_WARN("pricing_file_invalid", "path={} error={}", deps.pricing_file, error.what());
                pricing = nlohmann::json::array();
            }
        } else {
            LOG_WARN("pricing_file_unreadable", "path={} file_defaults_unavailable=true", deps.pricing_file);
        }
        for (const auto& name : deps.gw.coordinator.registry().list()) {
            const auto info = deps.gw.coordinator.registry().get_info_result(name);
            if (!info || (!info->prompt_price_per_million &&
                          !info->cached_prompt_price_per_million &&
                          !info->completion_price_per_million)) continue;
            auto existing = std::find_if(pricing.begin(), pricing.end(), [&](const nlohmann::json& entry) {
                return entry.value("model_name", "") == name;
            });
            nlohmann::json value = existing == pricing.end()
                ? nlohmann::json{
                    {"model_name", name},
                    {"currency", "USD"},
                    {"prompt_price_per_million",
                     info->prompt_price_per_million.value_or(0.0)},
                    {"cached_prompt_price_per_million",
                     info->cached_prompt_price_per_million.value_or(
                         info->prompt_price_per_million.value_or(0.0))},
                    {"completion_price_per_million",
                     info->completion_price_per_million.value_or(0.0)}}
                : *existing;
            if (info->prompt_price_per_million) {
                value["prompt_price_per_million"] =
                    *info->prompt_price_per_million;
            }
            if (info->cached_prompt_price_per_million) {
                value["cached_prompt_price_per_million"] =
                    *info->cached_prompt_price_per_million;
            }
            if (info->completion_price_per_million) {
                value["completion_price_per_million"] =
                    *info->completion_price_per_million;
            }
            value["source"] = "model_settings";
            if (existing == pricing.end()) pricing.push_back(std::move(value));
            else *existing = std::move(value);
        }
        for (const auto& alias : deps.gw.coordinator.registry().aliases()) {
            auto target = std::find_if(pricing.begin(), pricing.end(), [&](const nlohmann::json& entry) {
                return entry.value("model_name", "") == alias.target;
            });
            if (target == pricing.end()) continue;
            nlohmann::json value = *target;
            value["model_name"] = alias.name;
            value["source"] = "model_alias";
            auto existing = std::find_if(pricing.begin(), pricing.end(), [&](const nlohmann::json& entry) {
                return entry.value("model_name", "") == alias.name;
            });
            if (existing == pricing.end()) pricing.push_back(std::move(value));
            else *existing = std::move(value);
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

    server.Get(R"(^/api/events/stream$)", wrap([deps](const httplib::Request& req,
                                                 httplib::Response& resp) {
        (void)req;
        if (!deps.gw.events) {
            resp.status = 503;
            return;
        }
        auto lease = std::make_shared<DashboardStreamLease>();
        if (!lease->acquire()) {
            write_error(resp, 503, "too_many_event_streams",
                        "dashboard event stream limit reached");
            return;
        }
        auto sub = deps.gw.events->subscribe(64);
        LOG_INFO("sse_subscribe", "subscribers={}", deps.gw.events->subscriber_count());
        resp.set_header("Cache-Control", "no-cache");
        resp.set_header("X-Accel-Buffering", "no");
        resp.set_chunked_content_provider(
            "text/event-stream",
            [sub, lease](std::size_t, httplib::DataSink& sink) -> bool {
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
            [sub, lease, deps](bool) {
                sub->close();
                lease->release();
                LOG_INFO("sse_unsubscribe", "subscribers={}",
                         deps.gw.events ? deps.gw.events->subscriber_count() : 0);
            });
    }));
}

} // namespace inferdeck::gateway
