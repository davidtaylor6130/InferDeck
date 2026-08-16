#include "gateway/profile_benchmark_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace inferdeck::gateway {

namespace {

std::tm local_time(std::time_t value) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &value);
#else
    localtime_r(&value, &result);
#endif
    return result;
}

std::string date_key(const std::tm& value) {
    std::ostringstream stream;
    stream << std::put_time(&value, "%Y-%m-%d");
    return stream.str();
}

int minutes(const std::string& value) {
    return std::stoi(value.substr(0, 2)) * 60 + std::stoi(value.substr(3, 2));
}

double artifact_size_mb(const std::string& path) {
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    return error ? 0.0 : static_cast<double>(bytes) / (1024.0 * 1024.0);
}

std::int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t next_start_ms(const std::string& start, const std::string& end,
                           bool attempted_today) {
    const auto now = std::time(nullptr);
    auto local = local_time(now);
    const auto current_minutes = local.tm_hour * 60 + local.tm_min;
    local.tm_hour = minutes(start) / 60;
    local.tm_min = minutes(start) % 60;
    local.tm_sec = 0;
    auto next = std::mktime(&local);
    if (current_minutes >= minutes(start) && current_minutes < minutes(end) &&
        !attempted_today) {
        return static_cast<std::int64_t>(now) * 1000;
    }
    if (next <= now || attempted_today) {
        local.tm_mday += 1;
        next = std::mktime(&local);
    }
    return static_cast<std::int64_t>(next) * 1000;
}

}

ProfileBenchmarkScheduler::ProfileBenchmarkScheduler(
    ProfileBenchmarkManager& manager, model::BackendCoordinator& coordinator,
    observability::GpuTelemetry& gpu)
    : manager_(manager), coordinator_(coordinator), gpu_(gpu),
      worker_([this] { loop(); }) {}

ProfileBenchmarkScheduler::~ProfileBenchmarkScheduler() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
}

std::string ProfileBenchmarkScheduler::timezone_name() const {
    const auto now = std::time(nullptr);
    const auto local = local_time(now);
    std::ostringstream stream;
    stream << std::put_time(&local, "%Z");
    return stream.str();
}

std::vector<ScheduledOptimizationStatus> ProfileBenchmarkScheduler::statuses() const {
    std::vector<ScheduledOptimizationStatus> result;
    std::lock_guard lock(mutex_);
    for (const auto& name : coordinator_.registry().list()) {
        const auto info = coordinator_.registry().get_info_result(name);
        if (!info || info->runtime != "llama_cpp") continue;
        auto status = state_.contains(name) ? state_.at(name) : ScheduledOptimizationStatus{};
        status.model = name;
        status.enabled = info->optimization.schedule_enabled;
        status.window_start = info->optimization.schedule_window_start;
        status.window_end = info->optimization.schedule_window_end;
        const auto now = local_time(std::time(nullptr));
        const bool attempted_today = attempted_date_.contains(name) &&
            attempted_date_.at(name) == date_key(now);
        status.next_run_unix_ms = status.enabled
            ? next_start_ms(status.window_start, status.window_end,
                            attempted_today)
            : 0;
        result.push_back(std::move(status));
    }
    return result;
}

void ProfileBenchmarkScheduler::evaluate() {
    {
        std::lock_guard lock(mutex_);
        if (!in_flight_model_.empty()) {
            const auto snapshot = manager_.snapshot();
            if (snapshot.state != "running" && snapshot.state != "cancelling") {
                auto& status = state_[in_flight_model_];
                status.model = in_flight_model_;
                status.last_finished_unix_ms = snapshot.finished_unix_ms;
                status.last_outcome = snapshot.state;
                status.last_message = snapshot.message;
                in_flight_model_.clear();
            }
        }
    }
    if (manager_.snapshot().state == "running" || manager_.snapshot().state == "cancelling") return;
    const auto gpu = gpu_.latest();
    if (!gpu.available || gpu.vram_total_mb <= 0.0 || gpu.utilization_pct > 20.0) return;
    if (coordinator_.active_request_count() > 0 || coordinator_.queued_request_count() > 0 ||
        coordinator_.swap_in_progress()) return;
    const auto now = std::time(nullptr);
    const auto local = local_time(now);
    const auto today = date_key(local);
    const int minute = local.tm_hour * 60 + local.tm_min;
    for (const auto& name : coordinator_.registry().list()) {
        const auto info_result = coordinator_.registry().get_info_result(name);
        if (!info_result) continue;
        const auto& info = *info_result;
        if (info.runtime != "llama_cpp" || !info.optimization.schedule_enabled ||
            minute < minutes(info.optimization.schedule_window_start) ||
            minute >= minutes(info.optimization.schedule_window_end)) continue;
        {
            std::lock_guard lock(mutex_);
            if (attempted_date_[name] == today) continue;
        }
        optimize::ProfileInput input;
        input.model = name;
        input.total_vram_mb = gpu.vram_total_mb;
        input.model_file_mb = artifact_size_mb(info.gguf_path) +
            (info.has_vision ? artifact_size_mb(info.mmproj_path) : 0.0);
        input.configured_vram_mb = info.vram_required_mb;
        input.context_per_slot = info.context_size;
        input.slots = info.n_slots;
        input.min_slots = info.min_slots;
        input.n_batch = info.n_batch.value_or(512);
        input.n_ubatch = info.n_ubatch.value_or(input.n_batch);
        input.cache_type_k = info.cache_type_k.empty() ? "q8_0" : info.cache_type_k;
        input.cache_type_v = info.cache_type_v.empty() ? "q8_0" : info.cache_type_v;
        const auto started = manager_.start(info, input, 3);
        if (!started) return;
        std::lock_guard lock(mutex_);
        attempted_date_[name] = today;
        in_flight_model_ = name;
        auto& status = state_[name];
        status.model = name;
        status.last_started_unix_ms = started->started_unix_ms > 0
            ? started->started_unix_ms : unix_ms();
        status.last_outcome = "running";
        status.last_message = "Scheduled benchmark started";
        return;
    }
}

void ProfileBenchmarkScheduler::loop() {
    while (!stop_.load()) {
        evaluate();
        for (int i = 0; i < 150 && !stop_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }
}

}
