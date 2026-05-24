#pragma once

#include "llama_cpp/LlamaEngine.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace inferdeck::gateway {

class RuntimeActivity {
public:
    static RuntimeActivity& Get();

    std::string StartJob(const std::string& type,
                         const std::string& client,
                         const std::string& model,
                         const nlohmann::json& payload,
                         int priority = 50);
    void CompleteJob(const std::string& id, const inferdeck::core::InferenceResult& result, const nlohmann::json& response_preview);
    void FailJob(const std::string& id, const std::string& error, int status_code = 500);
    void CancelJob(const std::string& id);
    std::string RetryJob(const std::string& id);
    void SetQueuePaused(bool paused);
    void ClearFailedJobs();

    nlohmann::json JobsJson() const;
    nlohmann::json JobJson(const std::string& id) const;
    nlohmann::json JobEventsJson(const std::string& id) const;
    nlohmann::json JobResultJson(const std::string& id) const;
    nlohmann::json QueueJson() const;
    nlohmann::json SummaryJson() const;
    nlohmann::json ObservabilityJson() const;
    nlohmann::json SamplesJson() const;
    nlohmann::json LogsJson(std::size_t limit) const;

private:
    struct Event {
        std::string event_type;
        std::string message;
        std::string created_at;
        nlohmann::json data;
    };

    struct Job {
        std::string id;
        std::string type;
        std::string status;
        int priority = 50;
        std::string client;
        std::string model;
        std::string resource_class = "gpu_llm";
        std::string created_at;
        std::string started_at;
        std::string completed_at;
        std::uint64_t prompt_tokens = 0;
        std::uint64_t completion_tokens = 0;
        std::uint64_t total_tokens = 0;
        double duration_ms = 0;
        std::int64_t accepted_epoch_ms = 0;
        std::int64_t completed_epoch_ms = 0;
        int http_status = 200;
        std::string error;
        nlohmann::json payload;
        nlohmann::json result;
        std::vector<Event> events;
    };

    struct Sample {
        std::string timestamp;
        double queue_depth = 0;
        double running = 0;
        double tokens = 0;
        double latency_ms = 0;
    };

    RuntimeActivity() = default;
    nlohmann::json ToJson(const Job& job, bool include_details = true) const;
    void AddEvent(Job& job, const std::string& type, const std::string& message, nlohmann::json data = nlohmann::json::object());
    void AddSample(const Job& job);
    std::string MakeJobIdLocked() const;

    mutable std::mutex mutex_;
    std::vector<Job> jobs_;
    std::vector<Sample> samples_;
    bool queue_paused_ = false;
    std::uint64_t sequence_ = 0;
};

std::string RuntimeIsoNow();

} // namespace inferdeck::gateway
