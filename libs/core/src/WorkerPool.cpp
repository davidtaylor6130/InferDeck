/// @file WorkerPool.cpp
/// @brief Thread pool for processing inference jobs.

#include "core/WorkerPool.hpp"
#include "core/Logger.hpp"

namespace inferdeck::core {

WorkerPool::WorkerPool(int num_workers, int max_queue_size)
    : num_workers_(num_workers) {
    Logger::Get().Info("WorkerPool created: num_workers=%d, max_queue=%d",
                       num_workers, max_queue_size);
}

void WorkerPool::Start(JobQueue& queue, const std::function<void(const InferenceJob&)>& worker_fn) {
    if (running_.load()) {
        Logger::Get().Warn("WorkerPool already running");
        return;
    }

    Logger::Get().Info("WorkerPool starting %d workers", num_workers_);
    running_.store(true);

    for (int i = 0; i < num_workers_; ++i) {
        threads_.emplace_back(&WorkerPool::WorkerThread, this, std::ref(queue), worker_fn);
    }
}

void WorkerPool::Stop() {
    Logger::Get().Info("WorkerPool stopping");
    running_.store(false);

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    threads_.clear();
    Logger::Get().Info("WorkerPool stopped");
}

bool WorkerPool::IsRunning() const {
    return running_.load();
}

int WorkerPool::ActiveCount() const {
    return active_workers_.load();
}

int WorkerPool::WorkerCount() const {
    return num_workers_;
}

void WorkerPool::WorkerThread(JobQueue& queue, const std::function<void(const InferenceJob&)>& worker_fn) {
    Logger::Get().Info("WorkerThread started");

    while (running_.load()) {
        InferenceJob job;
        if (queue.Pop(job)) {
            active_workers_.fetch_add(1);
            Logger::Get().Info("Processing job: %s (priority=%d)",
                              job.id.c_str(), job.priority);

            try {
                worker_fn(job);
            } catch (const std::exception& e) {
                Logger::Get().Error("WorkerThread exception: %s", e.what());
            } catch (...) {
                Logger::Get().Error("WorkerThread unknown exception");
            }

            active_workers_.fetch_sub(1);
        }
    }

    Logger::Get().Info("WorkerThread stopped");
}

} // namespace inferdeck::core
