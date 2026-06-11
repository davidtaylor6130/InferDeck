#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace inferdeck::gateway {

struct SwapSnapshot {
    bool swapping{false};
    std::string target;
    std::string from;
    std::string last_error;
    std::int64_t started_unix_ms{0};
};

class SwapTracker {
public:
    bool begin(const std::string& from, const std::string& target, std::int64_t now_unix_ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (state_.swapping) return false;
        state_.swapping = true;
        state_.from = from;
        state_.target = target;
        state_.started_unix_ms = now_unix_ms;
        state_.last_error.clear();
        return true;
    }

    void end(bool success, const std::string& error) {
        std::lock_guard<std::mutex> lk(mtx_);
        state_.swapping = false;
        state_.last_error = success ? std::string{} : error;
    }

    [[nodiscard]] SwapSnapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return state_;
    }

private:
    mutable std::mutex mtx_;
    SwapSnapshot state_;
};

} // namespace inferdeck::gateway
