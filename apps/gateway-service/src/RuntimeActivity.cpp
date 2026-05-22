#include "RuntimeActivity.hpp"

#include <algorithm>
#include <ctime>
#include <sstream>

namespace inferdeck::gateway {
namespace {

std::string TodayPrefix() {
    return RuntimeIsoNow().substr(0, 10);
}

bool IsTerminal(const std::string& status) {
    return status == "succeeded" || status == "failed" || status == "cancelled" || status == "dead_letter";
}

} // namespace

std::string RuntimeIsoNow() {
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

RuntimeActivity& RuntimeActivity::Get() {
    static RuntimeActivity activity;
    return activity;
}

std::string RuntimeActivity::MakeJobIdLocked() const {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return "job-" + std::to_string(ms) + "-" + std::to_string(sequence_ + 1);
}

void RuntimeActivity::AddEvent(Job& job, const std::string& type, const std::string& message, nlohmann::json data) {
    job.events.push_back({type, message, RuntimeIsoNow(), std::move(data)});
    if (job.events.size() > 40) {
        job.events.erase(job.events.begin(), job.events.begin() + static_cast<std::ptrdiff_t>(job.events.size() - 40));
    }
}

void RuntimeActivity::AddSample(const Job& job) {
    std::size_t running = 0;
    std::size_t queued = 0;
    for (const auto& item : jobs_) {
        if (item.status == "running" || item.status == "leased") ++running;
        if (item.status == "queued") ++queued;
    }
    samples_.push_back({RuntimeIsoNow(), static_cast<double>(queued), static_cast<double>(running), static_cast<double>(job.total_tokens), job.duration_ms});
    if (samples_.size() > 240) {
        samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(samples_.size() - 240));
    }
}

std::string RuntimeActivity::StartJob(const std::string& type,
                                      const std::string& client,
                                      const std::string& model,
                                      const nlohmann::json& payload,
                                      int priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++sequence_;
    Job job;
    job.id = MakeJobIdLocked();
    job.type = type;
    job.status = queue_paused_ ? "paused" : "running";
    job.priority = priority;
    job.client = client.empty() ? "OpenAI compatible" : client;
    job.model = model;
    job.created_at = RuntimeIsoNow();
    job.started_at = job.status == "running" ? job.created_at : "";
    job.payload = payload;
    AddEvent(job, "queued", "Request accepted by gateway", {{"model", model}, {"type", type}});
    if (job.status == "running") {
        AddEvent(job, "started", "GPU LLM lease acquired", {{"resourceClass", job.resource_class}});
    }
    jobs_.insert(jobs_.begin(), std::move(job));
    if (jobs_.size() > 200) jobs_.pop_back();
    return jobs_.front().id;
}

void RuntimeActivity::CompleteJob(const std::string& id, const inferdeck::core::InferenceResult& result, const nlohmann::json& response_preview) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) { return job.id == id; });
    if (it == jobs_.end()) return;
    it->status = result.HasError() ? "failed" : "succeeded";
    it->completed_at = RuntimeIsoNow();
    it->prompt_tokens = static_cast<std::uint64_t>(std::max(0, result.prompt_tokens));
    it->completion_tokens = static_cast<std::uint64_t>(std::max(0, result.completion_tokens));
    it->total_tokens = static_cast<std::uint64_t>(std::max(0, result.total_tokens));
    it->duration_ms = result.duration_ms;
    it->http_status = result.http_status;
    it->result = response_preview;
    if (result.HasError()) it->error = result.error_message;
    AddEvent(*it, it->status == "succeeded" ? "completed" : "failed",
             it->status == "succeeded" ? "Request completed" : "Request failed",
             {{"promptTokens", it->prompt_tokens}, {"completionTokens", it->completion_tokens}, {"durationMs", it->duration_ms}});
    AddSample(*it);
}

void RuntimeActivity::FailJob(const std::string& id, const std::string& error, int status_code) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) { return job.id == id; });
    if (it == jobs_.end()) return;
    it->status = "failed";
    it->completed_at = RuntimeIsoNow();
    it->http_status = status_code;
    it->error = error;
    AddEvent(*it, "failed", error, {{"statusCode", status_code}});
    AddSample(*it);
}

void RuntimeActivity::CancelJob(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) { return job.id == id; });
    if (it == jobs_.end() || IsTerminal(it->status)) return;
    it->status = "cancelled";
    it->completed_at = RuntimeIsoNow();
    AddEvent(*it, "cancelled", "Cancellation requested from dashboard");
}

std::string RuntimeActivity::RetryJob(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) { return job.id == id; });
    if (it == jobs_.end()) return "";
    Job retry = *it;
    ++sequence_;
    retry.id = MakeJobIdLocked();
    retry.status = queue_paused_ ? "paused" : "queued";
    retry.created_at = RuntimeIsoNow();
    retry.started_at.clear();
    retry.completed_at.clear();
    retry.error.clear();
    retry.result = nullptr;
    retry.events.clear();
    AddEvent(retry, "queued", "Retry queued from dashboard", {{"sourceJobId", id}});
    jobs_.insert(jobs_.begin(), retry);
    return retry.id;
}

void RuntimeActivity::SetQueuePaused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_paused_ = paused;
}

void RuntimeActivity::ClearFailedJobs() {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Job& job) {
        return job.status == "failed" || job.status == "dead_letter";
    }), jobs_.end());
}

nlohmann::json RuntimeActivity::ToJson(const Job& job, bool include_details) const {
    nlohmann::json out = {
        {"id", job.id},
        {"type", job.type},
        {"status", job.status},
        {"priority", job.priority},
        {"client", job.client},
        {"model", job.model},
        {"resourceClass", job.resource_class},
        {"resource_class", job.resource_class},
        {"createdAt", job.created_at},
        {"created_at", job.created_at},
        {"startedAt", job.started_at.empty() ? nullptr : nlohmann::json(job.started_at)},
        {"started_at", job.started_at.empty() ? nullptr : nlohmann::json(job.started_at)},
        {"completedAt", job.completed_at.empty() ? nullptr : nlohmann::json(job.completed_at)},
        {"completed_at", job.completed_at.empty() ? nullptr : nlohmann::json(job.completed_at)},
        {"durationMs", job.duration_ms},
        {"promptTokens", job.prompt_tokens},
        {"completionTokens", job.completion_tokens},
        {"totalTokens", job.total_tokens},
        {"httpStatus", job.http_status},
        {"error", job.error.empty() ? nullptr : nlohmann::json(job.error)}
    };
    if (include_details) {
        out["payload"] = job.payload;
        out["result"] = job.result;
        out["events"] = nlohmann::json::array();
        for (const auto& event : job.events) {
            out["events"].push_back({{"eventType", event.event_type}, {"message", event.message}, {"createdAt", event.created_at}, {"data", event.data}});
        }
    }
    return out;
}

nlohmann::json RuntimeActivity::JobsJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json jobs = nlohmann::json::array();
    for (const auto& job : jobs_) jobs.push_back(ToJson(job, false));
    return jobs;
}

nlohmann::json RuntimeActivity::JobJson(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) { return job.id == id; });
    return it == jobs_.end() ? nlohmann::json() : ToJson(*it, true);
}

nlohmann::json RuntimeActivity::JobEventsJson(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) { return job.id == id; });
    nlohmann::json events = nlohmann::json::array();
    if (it != jobs_.end()) {
        for (const auto& event : it->events) {
            events.push_back({{"eventType", event.event_type}, {"message", event.message}, {"createdAt", event.created_at}, {"data", event.data}});
        }
    }
    return events;
}

nlohmann::json RuntimeActivity::JobResultJson(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) { return job.id == id; });
    return it == jobs_.end() ? nullptr : it->result;
}

nlohmann::json RuntimeActivity::QueueJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int queued = 0;
    int running = 0;
    int paused = queue_paused_ ? 1 : 0;
    int failed = 0;
    std::string lock_owner;
    for (const auto& job : jobs_) {
        if (job.status == "queued") ++queued;
        if (job.status == "running" || job.status == "leased") {
            ++running;
            if (lock_owner.empty()) lock_owner = job.model;
        }
        if (job.status == "paused") ++paused;
        if (job.status == "failed" || job.status == "dead_letter") ++failed;
    }
    return {
        {"queued", queued},
        {"running", running},
        {"paused", paused},
        {"failed", failed},
        {"totalQueued", queued},
        {"totalRunning", running},
        {"totalPaused", paused},
        {"totalFailed", failed},
        {"gpuLocked", running > 0},
        {"lockOwner", lock_owner.empty() ? nullptr : nlohmann::json(lock_owner)},
        {"policy", "single_gpu_fifo_with_priority"},
        {"pausedByAdmin", queue_paused_}
    };
}

nlohmann::json RuntimeActivity::SummaryJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto today = TodayPrefix();
    std::uint64_t jobs_today = 0;
    std::uint64_t succeeded_today = 0;
    std::uint64_t failed_today = 0;
    std::uint64_t prompt_tokens = 0;
    std::uint64_t completion_tokens = 0;
    std::uint64_t total_tokens = 0;
    double latency_sum = 0;
    std::uint64_t latency_count = 0;

    for (const auto& job : jobs_) {
        bool is_today = job.created_at.rfind(today, 0) == 0;
        if (is_today) {
            ++jobs_today;
            if (job.status == "succeeded") ++succeeded_today;
            if (job.status == "failed" || job.status == "dead_letter") ++failed_today;
        }
        prompt_tokens += job.prompt_tokens;
        completion_tokens += job.completion_tokens;
        total_tokens += job.total_tokens;
        if (job.duration_ms > 0) {
            latency_sum += job.duration_ms;
            ++latency_count;
        }
    }

    return {
        {"jobsToday", jobs_today},
        {"succeededToday", succeeded_today},
        {"failedToday", failed_today},
        {"promptTokens", prompt_tokens},
        {"completionTokens", completion_tokens},
        {"totalTokens", total_tokens},
        {"avgLatencyMs", latency_count == 0 ? 0 : latency_sum / latency_count},
        {"historyCount", jobs_.size()}
    };
}

nlohmann::json RuntimeActivity::SamplesJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json out = nlohmann::json::array();
    for (const auto& sample : samples_) {
        out.push_back({
            {"timestamp", sample.timestamp},
            {"queueDepth", sample.queue_depth},
            {"running", sample.running},
            {"tokens", sample.tokens},
            {"latencyMs", sample.latency_ms},
            {"metricValue", std::min(100.0, sample.tokens > 0 ? sample.tokens : sample.running * 25.0)}
        });
    }
    return out;
}

nlohmann::json RuntimeActivity::LogsJson(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json logs = nlohmann::json::array();
    for (const auto& job : jobs_) {
        for (const auto& event : job.events) {
            logs.push_back({
                {"timestamp", event.created_at},
                {"level", event.event_type == "failed" ? "error" : "info"},
                {"service", "scheduler"},
                {"jobId", job.id},
                {"message", event.message},
                {"data", event.data}
            });
        }
    }
    while (logs.size() > limit) logs.erase(logs.begin());
    return logs;
}

} // namespace inferdeck::gateway
