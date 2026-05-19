#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <string>
#include <chrono>

namespace inferdeck::core {

struct InferenceJob {
    std::string id;
    std::string model;
    std::string prompt;
    std::string messages_json;
    std::string params_json;
    int priority;
    std::chrono::steady_clock::time_point created_at;

    bool operator<(const InferenceJob& other) const {
        return priority < other.priority;
    }
};

using JobCallback = std::function<void(const InferenceJob&)>;

class JobQueue {
public:
    JobQueue(int max_size = 100, int worker_threads = 4);
    ~JobQueue() = default;

    bool Push(InferenceJob job);
    bool Pop(InferenceJob& job, int timeout_ms = 5000);
    int Size() const;
    int PendingCount() const;
    bool IsFull() const;
    int SubmittedCount() const;
    int CompletedCount() const;
    int FailedCount() const;
    void Shutdown();
    bool IsShutdown() const;

private:
    std::priority_queue<InferenceJob> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_{false};
    int max_size_;
    std::atomic<int> submitted_{0};
    std::atomic<int> completed_{0};
    std::atomic<int> failed_{0};
};

} // namespace inferdeck::core
