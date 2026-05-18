/// @file WorkerPool.hpp
/// @brief Thread pool for processing inference jobs.

#pragma once

#include "core/JobQueue.hpp"
#include <thread>
#include <functional>
#include <vector>
#include <atomic>

namespace inferdeck::core {

class WorkerPool {
public:
    WorkerPool(int num_workers = 4, int max_queue_size = 100);
    ~WorkerPool() = default;

    // Start the worker pool
    void Start(JobQueue& queue, const std::function<void(const InferenceJob&)>& worker_fn);

    // Stop the worker pool
    void Stop();

    // Check if pool is running
    bool IsRunning() const;

    // Get active worker count
    int ActiveCount() const;

    // Get total worker count
    int WorkerCount() const;

private:
    void WorkerThread(JobQueue& queue, const std::function<void(const InferenceJob&)>& worker_fn);

    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
    std::atomic<int> active_workers_{0};
    int num_workers_;
};

} // namespace inferdeck::core
