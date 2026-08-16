#include "gateway/profile_benchmark.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace inferdeck::gateway {

namespace {

constexpr double minimum_vram_reserve_mb = 2048.0;
constexpr double baseline_vram_tolerance_mb = 256.0;

std::int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string candidate_key(const optimize::ProfileCandidate& candidate) {
    return std::to_string(candidate.context_per_slot) + "|" +
        std::to_string(candidate.slots) + "|" +
        std::to_string(candidate.n_batch) + "|" +
        std::to_string(candidate.n_ubatch) + "|" +
        candidate.cache_type_k + "|" + candidate.cache_type_v + "|" +
        std::to_string(candidate.mtp_max_active_requests);
}

}

ProfileBenchmarkManager::ProfileBenchmarkManager(
    model::BackendCoordinator& coordinator,
    SwapTracker* swap_tracker,
    std::atomic<ComputeResource>& maintenance_resource,
    ProfileBenchmarkTrialRunner runner)
    : coordinator_(coordinator),
      swap_tracker_(swap_tracker),
      maintenance_resource_(maintenance_resource),
      runner_(std::move(runner)) {}

ProfileBenchmarkManager::~ProfileBenchmarkManager() {
    cancel_requested_.store(true);
    if (worker_.joinable()) worker_.join();
    maintenance_resource_.store(ComputeResource::None);
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
    const auto resource = model_compute_resource(model);
    ComputeResource expected = ComputeResource::None;
    if (!maintenance_resource_.compare_exchange_strong(expected, resource)) {
        return foundation::Err<ProfileBenchmarkSnapshot>(
            foundation::ErrorCode::AlreadyExists,
            "InferDeck is already in maintenance mode");
    }
    bool resource_busy = false;
    for (const auto& name : coordinator_.registry().list()) {
        const auto info = coordinator_.registry().get_info_result(name);
        if (info && model_compute_resource(*info) == resource &&
            coordinator_.active_request_count(name) > 0) {
            resource_busy = true;
            break;
        }
    }
    if (!resource_busy) {
        for (const auto& queued : coordinator_.queue()) {
            const auto info = coordinator_.registry().get_info_result(queued.model);
            if (info && model_compute_resource(*info) == resource) {
                resource_busy = true;
                break;
            }
        }
    }
    if (!resource_busy && resource == ComputeResource::Gpu && swap_tracker_ &&
        swap_tracker_->snapshot().swapping) {
        resource_busy = true;
    }
    if (resource_busy) {
        maintenance_resource_.store(ComputeResource::None);
        return foundation::Err<ProfileBenchmarkSnapshot>(
            foundation::ErrorCode::Unavailable,
            "benchmark requires no active or queued work on the same compute resource");
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
    maintenance_resource_.store(ComputeResource::None);
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
    const auto resource = model_compute_resource(model);
    std::vector<std::string> previous_models;
    for (const auto& name : coordinator_.get_loaded_models()) {
        const auto info = coordinator_.registry().get_info_result(name);
        if (info && model_compute_resource(*info) == resource) {
            previous_models.push_back(name);
        }
    }
    auto previous_primary = coordinator_.get_loaded_model();
    if (previous_primary &&
        std::find(previous_models.begin(), previous_models.end(),
                  *previous_primary) == previous_models.end()) {
        previous_primary.reset();
    }
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

        optimize::ProfileCandidate active_candidate;
        active_candidate.context_per_slot = input.context_per_slot;
        active_candidate.slots = input.slots;
        active_candidate.n_batch = input.n_batch;
        active_candidate.n_ubatch = input.n_ubatch;
        active_candidate.cache_type_k = input.cache_type_k;
        active_candidate.cache_type_v = input.cache_type_v;
        active_candidate.flash_attention = input.flash_attention;
        active_candidate.mtp_max_active_requests =
            model.mtp_max_active_requests;
        active_candidate.fits = true;
        auto estimate = optimize::recommend_profile(input);
        std::vector<optimize::ProfileCandidate> selected;
        std::set<std::string> seen;
        seen.insert(candidate_key(active_candidate));
        if (model.mtp_enabled) {
            for (const int window : {2, std::min(4, input.slots)}) {
                if (window > input.slots || window == model.mtp_max_active_requests) {
                    continue;
                }
                auto candidate = active_candidate;
                candidate.mtp_max_active_requests = window;
                candidate.reasons = {
                    "Measures adaptive MTP with up to " +
                        std::to_string(window) + " concurrent requests",
                };
                if (seen.insert(candidate_key(candidate)).second) {
                    selected.push_back(std::move(candidate));
                }
                if (static_cast<int>(selected.size()) >= candidate_limit) break;
            }
        }
        for (const auto& candidate : estimate.candidates) {
            if (static_cast<int>(selected.size()) >= candidate_limit) break;
            auto measured_candidate = candidate;
            measured_candidate.mtp_max_active_requests =
                model.mtp_max_active_requests;
            if (!measured_candidate.fits ||
                !seen.insert(candidate_key(measured_candidate)).second) {
                continue;
            }
            selected.push_back(std::move(measured_candidate));
        }
        if (selected.empty()) {
            restore();
            finish("failed", "No safe candidate was available to benchmark",
                   restored);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.total_candidates = static_cast<int>(selected.size()) + 1;
        }

        const auto suite = prompts();
        update_stage("baseline", "Measuring the current active profile");
        ProfileBenchmarkTrial baseline;
        baseline.candidate = active_candidate;
        const auto baseline_progress = [this](
            const std::string& stage, const std::string& message) {
            update_stage("baseline_" + stage, "Current profile: " + message);
        };
        auto baseline_metrics = runner_(
            model, baseline.candidate, suite, cancel_requested_, baseline_progress);
        if (!baseline_metrics) {
            baseline.error = baseline_metrics.error().message;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.baseline = baseline;
                state_.has_baseline = true;
            }
            restore();
            finish(cancel_requested_.load() ? "cancelled" : "failed",
                   cancel_requested_.load()
                       ? "Benchmark cancelled while measuring the current profile"
                       : "Could not measure the current profile: " + baseline.error,
                   restored);
            return;
        }
        baseline.metrics = std::move(*baseline_metrics);
        baseline.completed = true;
        baseline.candidate.quality_score = baseline.metrics.quality_score;
        baseline.candidate.estimated_vram_mb = baseline.metrics.peak_vram_mb;
        baseline.candidate.reserve_vram_mb =
            input.total_vram_mb - baseline.metrics.peak_vram_mb;
        baseline.candidate.fits = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.baseline = std::move(baseline);
            state_.has_baseline = true;
            state_.completed_candidates = 1;
            state_.progress_pct = 90.0 /
                static_cast<double>(state_.total_candidates);
        }
        for (std::size_t index = 0; index < selected.size(); ++index) {
            if (cancel_requested_.load()) break;
            update_stage(
                "benchmarking",
                "Running measured candidate " + std::to_string(index + 1) +
                " of " + std::to_string(selected.size()));
            const auto progress =
                [this, index, total = selected.size() + 1](
                    const std::string& stage, const std::string& message) {
                    const double stage_fraction =
                        stage == "loading" ? 0.10 :
                        stage == "quality" ? 0.45 :
                        stage == "speed" ? 0.65 :
                        stage == "parallelism" ? 0.80 : 0.0;
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_.stage = stage;
                    state_.message = message;
                    state_.progress_pct =
                        ((static_cast<double>(index + 1) + stage_fraction) /
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
                    trial.candidate.reserve_vram_mb >=
                        minimum_vram_reserve_mb ||
                    trial.metrics.peak_vram_mb <=
                        state_.baseline.metrics.peak_vram_mb +
                            baseline_vram_tolerance_mb;
                trial.candidate.quality_score =
                    trial.metrics.quality_score;
                trial.candidate.reasons = {
                    "Measured with " +
                        std::to_string(trial.candidate.context_per_slot) +
                        " context tokens per slot across " +
                        std::to_string(trial.candidate.slots) + " slot(s)",
                    "Measured " + trial.candidate.cache_type_k + "/" +
                        trial.candidate.cache_type_v +
                        " KV cache precision",
                    "Actual peak VRAM leaves about " +
                        std::to_string(static_cast<int>(std::round(
                            trial.candidate.reserve_vram_mb))) +
                        " MB reserve",
                    "Passed " +
                        std::to_string(trial.metrics.quality_passes) +
                        " of " +
                        std::to_string(trial.metrics.quality_total) +
                        " fixed quality probes",
                    "Measured adaptive MTP up to " +
                        std::to_string(
                            trial.candidate.mtp_max_active_requests) +
                        " concurrent request(s)",
                };
                if (trial.candidate.reserve_vram_mb <
                    minimum_vram_reserve_mb && trial.candidate.fits) {
                    trial.candidate.reasons.push_back(
                        "Peak VRAM remains within 256 MB of the current active profile");
                }
            } else {
                trial.error = measured.error().message;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.trials.push_back(std::move(trial));
                state_.completed_candidates =
                    static_cast<int>(state_.trials.size()) + 1;
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
            state_.baseline.metrics.performance_index = 100.0;
            for (auto& trial : state_.trials) {
                if (!trial.completed) continue;
                const double prompt_ratio =
                    state_.baseline.metrics.prompt_tokens_per_second > 0.0
                        ? trial.metrics.prompt_tokens_per_second /
                            state_.baseline.metrics.prompt_tokens_per_second
                        : 1.0;
                const double generation_ratio =
                    state_.baseline.metrics.average_tokens_per_second > 0.0
                        ? trial.metrics.average_tokens_per_second /
                            state_.baseline.metrics.average_tokens_per_second
                        : 1.0;
                trial.metrics.performance_index =
                    (prompt_ratio + generation_ratio) * 50.0;
                trial.candidate.speed_score = generation_ratio;
                double concurrency_ratio_total = 0.0;
                int concurrency_ratio_count = 0;
                for (const auto& measured : trial.metrics.concurrency) {
                    const auto baseline_measurement = std::find_if(
                        state_.baseline.metrics.concurrency.begin(),
                        state_.baseline.metrics.concurrency.end(),
                        [&](const auto& value) {
                            return value.requests == measured.requests;
                        });
                    if (baseline_measurement !=
                            state_.baseline.metrics.concurrency.end() &&
                        baseline_measurement->aggregate_tokens_per_second > 0.0) {
                        concurrency_ratio_total +=
                            measured.aggregate_tokens_per_second /
                            baseline_measurement->aggregate_tokens_per_second;
                        ++concurrency_ratio_count;
                    }
                }
                trial.candidate.parallelism_score = concurrency_ratio_count > 0
                    ? concurrency_ratio_total /
                        static_cast<double>(concurrency_ratio_count)
                    : (state_.baseline.metrics.parallel_tokens_per_second > 0.0
                        ? trial.metrics.parallel_tokens_per_second /
                            state_.baseline.metrics.parallel_tokens_per_second
                        : 1.0);
                trial.candidate.headroom_score = std::clamp(
                    trial.candidate.reserve_vram_mb /
                        (input.total_vram_mb * 0.25),
                    0.0, 1.0);
                trial.candidate.overall_score =
                    trial.metrics.performance_index / 200.0 +
                    trial.candidate.parallelism_score * 0.5;
                if (model.mtp_enabled &&
                    trial.candidate.mtp_max_active_requests > 1) {
                    const auto verification = std::find_if(
                        trial.metrics.concurrency.begin(),
                        trial.metrics.concurrency.end(),
                        [&](const auto& value) {
                            return value.requests ==
                                trial.candidate.mtp_max_active_requests;
                        });
                    if (verification == trial.metrics.concurrency.end() ||
                        verification->mtp_requests != verification->requests) {
                        trial.candidate.fits = false;
                        trial.candidate.reasons.push_back(
                            "Rejected because concurrent MTP drafting was not verified for every request");
                    }
                }
                if (trial.metrics.quality_score + 0.0001 <
                    state_.baseline.metrics.quality_score) {
                    trial.candidate.fits = false;
                    trial.candidate.reasons.push_back(
                        "Rejected because correctness probes regressed from the current profile");
                }
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
                                    left.candidate.parallelism_score) <
                           std::tie(right.candidate.overall_score,
                                    right.candidate.parallelism_score);
                });
            if (best != state_.trials.end() && best->completed &&
                best->candidate.fits) {
                state_.recommended = best->candidate;
                state_.has_recommendation = true;
            }
        }
        if (!snapshot().has_recommendation) {
            finish("failed",
                   "Measured trials produced no faster candidate that preserved correctness and VRAM reserve",
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
