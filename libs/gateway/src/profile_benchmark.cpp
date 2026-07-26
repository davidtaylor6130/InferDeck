#include "gateway/profile_benchmark.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace inferdeck::gateway {

namespace {

std::int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string candidate_key(const optimize::ProfileCandidate& candidate) {
    return std::to_string(candidate.context_per_slot) + "|" +
        std::to_string(candidate.slots) + "|" +
        std::to_string(candidate.n_batch) + "|" +
        std::to_string(candidate.n_ubatch) + "|" +
        candidate.cache_type_k + "|" + candidate.cache_type_v;
}

}

ProfileBenchmarkManager::ProfileBenchmarkManager(
    model::BackendCoordinator& coordinator,
    SwapTracker* swap_tracker,
    std::atomic<bool>& maintenance_mode,
    ProfileBenchmarkTrialRunner runner)
    : coordinator_(coordinator),
      swap_tracker_(swap_tracker),
      maintenance_mode_(maintenance_mode),
      runner_(std::move(runner)) {}

ProfileBenchmarkManager::~ProfileBenchmarkManager() {
    cancel_requested_.store(true);
    if (worker_.joinable()) worker_.join();
    maintenance_mode_.store(false);
}

foundation::Result<ProfileBenchmarkSnapshot> ProfileBenchmarkManager::start(
    const model::ModelInfo& model,
    const optimize::ProfileInput& input,
    int candidate_limit) {
    if (!runner_) {
        return foundation::Err<ProfileBenchmarkSnapshot>(
            foundation::ErrorCode::Unavailable,
            "measured benchmark runner is unavailable");
    }
    if (candidate_limit < 1 || candidate_limit > 4) {
        return foundation::Err<ProfileBenchmarkSnapshot>(
            foundation::ErrorCode::InvalidArgument,
            "candidateLimit must be between 1 and 4");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.state == "running" || state_.state == "cancelling") {
            return foundation::Err<ProfileBenchmarkSnapshot>(
                foundation::ErrorCode::AlreadyExists,
                "a measured profile benchmark is already running");
        }
    }
    if (maintenance_mode_.exchange(true)) {
        return foundation::Err<ProfileBenchmarkSnapshot>(
            foundation::ErrorCode::AlreadyExists,
            "InferDeck is already in maintenance mode");
    }
    if (coordinator_.active_request_count() > 0 ||
        coordinator_.queued_request_count() > 0 ||
        (swap_tracker_ && swap_tracker_->snapshot().swapping)) {
        maintenance_mode_.store(false);
        return foundation::Err<ProfileBenchmarkSnapshot>(
            foundation::ErrorCode::Unavailable,
            "benchmark requires zero active requests, zero queued requests, and no model swap");
    }
    if (worker_.joinable()) worker_.join();
    cancel_requested_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {};
        state_.id = next_id_++;
        state_.state = "running";
        state_.stage = "preparing";
        state_.message = "Preparing measured benchmark";
        state_.model = model.name;
        state_.started_unix_ms = unix_ms();
    }
    worker_ = std::thread(
        [this, model, input, candidate_limit] {
            run(model, input, candidate_limit);
        });
    return foundation::Ok(snapshot());
}

ProfileBenchmarkSnapshot ProfileBenchmarkManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto copy = state_;
    copy.cancel_requested = cancel_requested_.load();
    return copy;
}

foundation::Result<void> ProfileBenchmarkManager::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.state != "running" && state_.state != "cancelling") {
        return foundation::Err<void>(
            foundation::ErrorCode::NotFound,
            "no measured profile benchmark is running");
    }
    cancel_requested_.store(true);
    state_.state = "cancelling";
    state_.message = "Cancelling after the current model operation";
    return foundation::Ok();
}

bool ProfileBenchmarkManager::wait_for_completion(
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto current = snapshot();
        if (current.state != "running" && current.state != "cancelling") {
            if (worker_.joinable()) worker_.join();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

void ProfileBenchmarkManager::update_stage(
    const std::string& stage, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.stage = stage;
    state_.message = message;
}

void ProfileBenchmarkManager::finish(
    const std::string& state, const std::string& message, bool restored) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.state = state;
        state_.stage = "finished";
        state_.message = message;
        state_.finished_unix_ms = unix_ms();
        state_.restored = restored;
        state_.progress_pct = 100.0;
    }
    maintenance_mode_.store(false);
}

std::vector<ProfileBenchmarkPrompt> ProfileBenchmarkManager::prompts() const {
    std::string retrieval =
        "Read the following records and return only the access word for record 17. ";
    for (int index = 1; index <= 32; ++index) {
        retrieval += "Record " + std::to_string(index) + " access word is " +
            (index == 17 ? std::string{"saffron"} :
             "filler" + std::to_string(index)) + ". ";
    }
    return {
        {"arithmetic",
         "Calculate (37 * 19) + 11. Return only the integer answer.",
         "714", 32},
        {"logic",
         "Mira is older than Noor. Noor is older than Cara. "
         "Return only the name of the youngest person.",
         "cara", 32},
        {"retrieval", std::move(retrieval), "saffron", 48},
    };
}

void ProfileBenchmarkManager::run(
    model::ModelInfo model,
    optimize::ProfileInput input,
    int candidate_limit) {
    const auto previous_models = coordinator_.get_loaded_models();
    const auto previous_primary = coordinator_.get_loaded_model();
    bool restored = false;
    auto restore = [&] {
        update_stage("restoring", "Restoring the previous model residency");
        bool ok = true;
        for (const auto& name : previous_models) {
            if (previous_primary && name == *previous_primary) continue;
            const auto result = coordinator_.load(name);
            if (!result) ok = false;
        }
        if (previous_primary) {
            const auto result = coordinator_.load(*previous_primary);
            if (!result) ok = false;
        }
        restored = ok;
    };

    try {
        update_stage("unloading", "Unloading resident models for isolated trials");
        for (const auto& name : previous_models) {
            const auto unloaded = coordinator_.unload(name);
            if (!unloaded) {
                restore();
                finish("failed", "Could not unload " + name + ": " +
                       unloaded.error().message, restored);
                return;
            }
        }
        if (cancel_requested_.load()) {
            restore();
            finish("cancelled", "Benchmark cancelled before the first trial",
                   restored);
            return;
        }

        auto estimate = optimize::recommend_profile(input);
        std::vector<optimize::ProfileCandidate> selected;
        std::set<std::string> seen;
        for (const auto& candidate : estimate.candidates) {
            if (!candidate.fits || !seen.insert(candidate_key(candidate)).second) {
                continue;
            }
            selected.push_back(candidate);
            if (static_cast<int>(selected.size()) >= candidate_limit) break;
        }
        if (selected.empty()) {
            restore();
            finish("failed", "No safe candidate was available to benchmark",
                   restored);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.total_candidates = static_cast<int>(selected.size());
        }

        const auto suite = prompts();
        for (std::size_t index = 0; index < selected.size(); ++index) {
            if (cancel_requested_.load()) break;
            update_stage(
                "benchmarking",
                "Running measured candidate " + std::to_string(index + 1) +
                " of " + std::to_string(selected.size()));
            const auto progress =
                [this, index, total = selected.size()](
                    const std::string& stage, const std::string& message) {
                    const double stage_fraction =
                        stage == "loading" ? 0.10 :
                        stage == "quality" ? 0.45 :
                        stage == "parallelism" ? 0.80 : 0.0;
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_.stage = stage;
                    state_.message = message;
                    state_.progress_pct =
                        ((static_cast<double>(index) + stage_fraction) /
                         static_cast<double>(total)) * 90.0;
                };
            auto measured =
                runner_(model, selected[index], suite,
                        cancel_requested_, progress);
            ProfileBenchmarkTrial trial;
            trial.candidate = selected[index];
            if (measured) {
                trial.metrics = std::move(*measured);
                trial.completed = true;
                trial.candidate.estimated_vram_mb =
                    trial.metrics.peak_vram_mb;
                trial.candidate.reserve_vram_mb =
                    input.total_vram_mb - trial.metrics.peak_vram_mb;
                trial.candidate.fits =
                    trial.metrics.peak_vram_mb <= input.total_vram_mb * 0.88;
                trial.candidate.quality_score =
                    trial.metrics.quality_score;
            } else {
                trial.error = measured.error().message;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.trials.push_back(std::move(trial));
                state_.completed_candidates =
                    static_cast<int>(state_.trials.size());
                state_.progress_pct =
                    (static_cast<double>(state_.completed_candidates) /
                     static_cast<double>(state_.total_candidates)) * 90.0;
            }
        }

        restore();
        if (cancel_requested_.load()) {
            finish("cancelled", "Benchmark cancelled; previous models restored",
                   restored);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            double max_speed = 0.0;
            double max_parallel = 0.0;
            for (const auto& trial : state_.trials) {
                if (!trial.completed || !trial.candidate.fits) continue;
                max_speed = std::max(
                    max_speed, trial.metrics.average_tokens_per_second);
                max_parallel = std::max(
                    max_parallel, trial.metrics.parallel_tokens_per_second);
            }
            for (auto& trial : state_.trials) {
                if (!trial.completed) continue;
                trial.candidate.speed_score =
                    max_speed > 0.0
                        ? trial.metrics.average_tokens_per_second / max_speed
                        : 0.0;
                trial.candidate.parallelism_score =
                    max_parallel > 0.0
                        ? trial.metrics.parallel_tokens_per_second / max_parallel
                        : 0.0;
                trial.candidate.headroom_score = std::clamp(
                    trial.candidate.reserve_vram_mb /
                        (input.total_vram_mb * 0.25),
                    0.0, 1.0);
                trial.candidate.overall_score =
                    0.60 * trial.candidate.quality_score +
                    0.15 * trial.candidate.speed_score +
                    0.15 * trial.candidate.parallelism_score +
                    0.10 * trial.candidate.headroom_score;
                if (!trial.candidate.fits) {
                    trial.candidate.overall_score *= 0.25;
                }
            }
            auto best = std::max_element(
                state_.trials.begin(), state_.trials.end(),
                [](const ProfileBenchmarkTrial& left,
                   const ProfileBenchmarkTrial& right) {
                    if (left.completed != right.completed) {
                        return !left.completed;
                    }
                    if (left.candidate.fits != right.candidate.fits) {
                        return !left.candidate.fits;
                    }
                    return std::tie(left.candidate.overall_score,
                                    left.candidate.quality_score) <
                           std::tie(right.candidate.overall_score,
                                    right.candidate.quality_score);
                });
            if (best != state_.trials.end() && best->completed &&
                best->candidate.fits) {
                state_.recommended = best->candidate;
                state_.has_recommendation = true;
            }
        }
        if (!snapshot().has_recommendation) {
            finish("failed",
                   "Measured trials produced no candidate with 12 percent VRAM reserve",
                   restored);
            return;
        }
        finish("completed",
               "Measured benchmark complete; review the winning profile",
               restored);
    } catch (const std::exception& error) {
        restore();
        finish("failed", error.what(), restored);
    }
}

}
