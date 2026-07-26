#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace inferdeck::gateway {

struct SwapSnapshot {
    bool swapping{false};
    std::string target;
    std::string from;
    std::string last_error;
    bool last_cancelled{false};
    std::int64_t started_unix_ms{0};
};

class SwapTracker {
public:
    enum class StartResult {
        Started,
        Busy,
        Failed,
    };

    SwapTracker() = default;
    SwapTracker(const SwapTracker&) = delete;
    SwapTracker& operator=(const SwapTracker&) = delete;

    ~SwapTracker() {
        join();
    }

    StartResult start(const std::string& from,
                      const std::string& target,
                      std::int64_t now_unix_ms,
                      std::function<void()> task,
                      std::string& error) {
        std::thread completed;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (state_.swapping) return StartResult::Busy;
            if (worker_.joinable()) completed = std::move(worker_);
            state_.swapping = true;
            state_.from = from;
            state_.target = target;
            state_.started_unix_ms = now_unix_ms;
            state_.last_error.clear();
            state_.last_cancelled = false;
        }
        if (completed.joinable()) completed.join();

        try {
            std::lock_guard<std::mutex> lock(mtx_);
            worker_ = std::thread(
                [this, task = std::move(task)]() mutable {
                    try {
                        task();
                    } catch (const std::exception& exception) {
                        end(false, exception.what());
                    } catch (...) {
                        end(false, "swap task threw unknown exception");
                    }
                });
        } catch (const std::exception& exception) {
            error = exception.what();
            end(false, error);
            return StartResult::Failed;
        } catch (...) {
            error = "unknown thread launch failure";
            end(false, error);
            return StartResult::Failed;
        }
        return StartResult::Started;
    }

    void end(bool success, const std::string& error, bool cancelled = false) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            state_.swapping = false;
            state_.last_error = success ? std::string{} : error;
            state_.last_cancelled = !success && cancelled;
        }
        cv_.notify_all();
    }

    [[nodiscard]] SwapSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return state_;
    }

    bool wait_until_idle(std::chrono::steady_clock::time_point deadline) const {
        std::unique_lock<std::mutex> lock(mtx_);
        return cv_.wait_until(lock, deadline, [this] {
            return !state_.swapping;
        });
    }

    void join() {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (worker_.joinable()) worker = std::move(worker_);
        }
        if (worker.joinable()) worker.join();
    }

private:
    mutable std::mutex mtx_;
    mutable std::condition_variable cv_;
    SwapSnapshot state_;
    std::thread worker_;
};

} // namespace inferdeck::gateway
