#pragma once

#include "foundation/result.hpp"
#include "gateway/routes.hpp"
#include "gateway/swap_tracker.hpp"
#include "model/backend_coordinator.hpp"
#include "model/model_info.hpp"
#include "optimize/profile_optimizer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace inferdeck::gateway {

struct ProfileBenchmarkPrompt {
    std::string id;
    std::string prompt;
    std::string reference;
    int max_tokens{64};
};

struct ProfileBenchmarkConcurrencyMetrics {
    int requests{0};
    double aggregate_tokens_per_second{0.0};
    double average_request_tokens_per_second{0.0};
    int mtp_requests{0};
    int mtp_drafted_tokens{0};
    int mtp_accepted_tokens{0};
};

struct ProfileBenchmarkTrialMetrics {
    double load_ms{0.0};
    double prompt_tokens_per_second{0.0};
    double average_tokens_per_second{0.0};
    double parallel_tokens_per_second{0.0};
    double average_time_to_first_token_ms{0.0};
    double peak_vram_mb{0.0};
    double quality_score{0.0};
    double performance_index{100.0};
    int prompt_tokens{0};
    int completion_tokens{0};
    int quality_passes{0};
    int quality_total{0};
    std::vector<ProfileBenchmarkConcurrencyMetrics> concurrency;
    std::vector<std::string> output_samples;
};

struct ProfileBenchmarkTrial {
    optimize::ProfileCandidate candidate;
    ProfileBenchmarkTrialMetrics metrics;
    bool completed{false};
    std::string error;
};

struct ProfileBenchmarkSnapshot {
    std::uint64_t id{0};
    std::string state{"idle"};
    std::string stage{"idle"};
    std::string message;
    std::string model;
    int completed_candidates{0};
    int total_candidates{0};
    double progress_pct{0.0};
    std::int64_t started_unix_ms{0};
    std::int64_t finished_unix_ms{0};
    bool measured{true};
    bool cancel_requested{false};
    bool restored{false};
    bool has_baseline{false};
    bool has_recommendation{false};
    ProfileBenchmarkTrial baseline;
    optimize::ProfileCandidate recommended;
    std::vector<ProfileBenchmarkTrial> trials;
};

using ProfileBenchmarkProgress =
    std::function<void(const std::string&, const std::string&)>;
using ProfileBenchmarkTrialRunner = std::function<
    foundation::Result<ProfileBenchmarkTrialMetrics>(
        const model::ModelInfo&,
        const optimize::ProfileCandidate&,
        const std::vector<ProfileBenchmarkPrompt>&,
        const std::atomic<bool>&,
        const ProfileBenchmarkProgress&)>;

class ProfileBenchmarkManager {
public:
    ProfileBenchmarkManager(model::BackendCoordinator& coordinator,
                            SwapTracker* swap_tracker,
                            std::atomic<ComputeResource>& maintenance_resource,
                            ProfileBenchmarkTrialRunner runner);
    ~ProfileBenchmarkManager();

    ProfileBenchmarkManager(const ProfileBenchmarkManager&) = delete;
    ProfileBenchmarkManager& operator=(const ProfileBenchmarkManager&) = delete;

    foundation::Result<ProfileBenchmarkSnapshot> start(
        const model::ModelInfo& model,
        const optimize::ProfileInput& input,
        int candidate_limit = 3);
    ProfileBenchmarkSnapshot snapshot() const;
    foundation::Result<void> cancel();
    bool wait_for_completion(std::chrono::milliseconds timeout);

private:
    void run(model::ModelInfo model,
             optimize::ProfileInput input,
             int candidate_limit);
    void update_stage(const std::string& stage, const std::string& message);
    void finish(const std::string& state, const std::string& message,
                bool restored);
    std::vector<ProfileBenchmarkPrompt> prompts() const;

    model::BackendCoordinator& coordinator_;
    SwapTracker* swap_tracker_{nullptr};
    std::atomic<ComputeResource>& maintenance_resource_;
    ProfileBenchmarkTrialRunner runner_;
    mutable std::mutex mutex_;
    ProfileBenchmarkSnapshot state_;
    std::thread worker_;
    std::atomic<bool> cancel_requested_{false};
    std::uint64_t next_id_{1};
};

}
