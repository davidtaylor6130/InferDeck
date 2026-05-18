/// @file JobQueue.hpp
/// @brief Priority-based job queue for inference requests.

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
    std::vector<ChatMessage> messages;
    InferenceParams params;
    int priority;  // Higher = more urgent
    std::chrono::steady_clock::time_point created_at;
};

using JobCallback = std::function<void(const InferenceJob&)>;

class JobQueue {
public:
    JobQueue(int max_size = 100, int worker_threads = 4);
    ~JobQueue() = default;

    // Push a job into the queue
    bool Push(InferenceJob job);

    // Pop a job from the queue (blocks if empty)
    bool Pop(InferenceJob& job, int timeout_ms = 5000);

    // Get queue size
    int Size() const;

    // Get pending count
    int PendingCount() const;

    // Check if queue is full
    bool IsFull() const;

    // Get metrics
    int SubmittedCount() const;
    int CompletedCount() const;
    int FailedCount() const;

    // Shutdown the queue
    void Shutdown();

    // Check if shutdown requested
    bool IsShutdown() const;

private:
    std::priority_queue<InferenceJob> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_{false};
    int max_size_;
    int submitted_{0};
    int completed_{0};
    int failed_{0};
};

} // namespace inferdeck::core
