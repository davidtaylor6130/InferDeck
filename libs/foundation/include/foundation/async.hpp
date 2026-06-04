#pragma once

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace inferdeck::foundation {

template <typename F>
auto run_async(F&& f)
    -> std::future<std::invoke_result_t<std::decay_t<F>>> {
    using R = std::invoke_result_t<std::decay_t<F>>;
    std::promise<R> promise;
    auto future = promise.get_future();
    std::thread([p = std::move(promise), fn = std::forward<F>(f)]() mutable {
        try {
            if constexpr (std::is_void_v<R>) {
                fn();
                p.set_value();
            } else {
                p.set_value(fn());
            }
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    }).detach();
    return future;
}

class StopWatch {
public:
    StopWatch() : start_(std::chrono::steady_clock::now()) {}

    void reset() { start_ = std::chrono::steady_clock::now(); }

    template <typename Unit = std::chrono::milliseconds>
    [[nodiscard]] Unit elapsed() const {
        return std::chrono::duration_cast<Unit>(
            std::chrono::steady_clock::now() - start_);
    }

    [[nodiscard]] double elapsed_ms() const {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start_).count();
    }

    [[nodiscard]] double elapsed_seconds() const {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

} // namespace inferdeck::foundation
