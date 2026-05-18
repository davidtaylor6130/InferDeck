/// @file JobQueue.cpp
/// @brief Priority-based job queue implementation.

#include "core/JobQueue.hpp"
#include "core/Logger.hpp"

namespace inferdeck::core {

JobQueue::JobQueue(int max_size, int worker_threads)
    : max_size_(max_size) {
    Logger::Get().Info("JobQueue initialized: max_size=%d, workers=%d", max_size, worker_threads);
}

bool JobQueue::Push(InferenceJob job) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (shutdown_.load()) {
        Logger::Get().Warn("JobQueue is shutting down, rejecting job");
        return false;
    }

    if (static_cast<int>(queue_.size()) >= max_size_) {
        Logger::Get().Warn("JobQueue is full (%d)", max_size_);
        return false;
    }

    queue_.push(std::move(job));
    submitted_.fetch_add(1);
    cv_.notify_one();

    return true;
}

bool JobQueue::Pop(InferenceJob& job, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this] { return !queue_.empty() || shutdown_.load(); })) {
        return false;
    }

    if (shutdown_.load() || queue_.empty()) {
        return false;
    }

    job = queue_.top();
    queue_.pop();
    completed_.fetch_add(1);

    return true;
}

int JobQueue::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(queue_.size());
}

int JobQueue::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(queue_.size());
}

bool JobQueue::IsFull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(queue_.size()) >= max_size_;
}

int JobQueue::SubmittedCount() const {
    return submitted_.load();
}

int JobQueue::CompletedCount() const {
    return completed_.load();
}

int JobQueue::FailedCount() const {
    return failed_.load();
}

void JobQueue::Shutdown() {
    Logger::Get().Info("JobQueue shutting down");
    shutdown_.store(true);
    cv_.notify_all();
}

bool JobQueue::IsShutdown() const {
    return shutdown_.load();
}

} // namespace inferdeck::core
