#pragma once

#include "gateway/profile_benchmark.hpp"
#include "observability/gpu_telemetry.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace inferdeck::gateway {

struct ScheduledOptimizationStatus {
    std::string model;
    bool enabled{false};
    std::string window_start{"03:00"};
    std::string window_end{"04:00"};
    std::int64_t next_run_unix_ms{0};
    std::int64_t last_started_unix_ms{0};
    std::int64_t last_finished_unix_ms{0};
    std::string last_outcome{"never"};
    std::string last_message;
};

class ProfileBenchmarkScheduler {
public:
    ProfileBenchmarkScheduler(ProfileBenchmarkManager& manager,
                              model::BackendCoordinator& coordinator,
                              observability::GpuTelemetry& gpu);
    ~ProfileBenchmarkScheduler();

    ProfileBenchmarkScheduler(const ProfileBenchmarkScheduler&) = delete;
    ProfileBenchmarkScheduler& operator=(const ProfileBenchmarkScheduler&) = delete;

    [[nodiscard]] std::vector<ScheduledOptimizationStatus> statuses() const;
    [[nodiscard]] std::string timezone_name() const;
    void evaluate();

private:
    void loop();

    ProfileBenchmarkManager& manager_;
    model::BackendCoordinator& coordinator_;
    observability::GpuTelemetry& gpu_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ScheduledOptimizationStatus> state_;
    std::unordered_map<std::string, std::string> attempted_date_;
    std::string in_flight_model_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

}
