#pragma once

#include <chrono>
#include <future>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

namespace inferdeck::foundation {

template <typename F>
auto run_async(F&& f)
    -> std::future<std::invoke_result_t<std::decay_t<F>>> {
    return std::async(std::launch::async,
                      [fn = std::forward<F>(f)]() mutable
                          -> std::invoke_result_t<std::decay_t<F>> {
                          return std::invoke(std::move(fn));
                      });
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
