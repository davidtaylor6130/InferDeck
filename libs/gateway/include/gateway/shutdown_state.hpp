#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

namespace inferdeck::gateway {

enum class ShutdownPhase {
    Running,
    Cancelling,
    Draining,
    Complete,
    TimedOut,
};

class ShutdownStateMachine {
public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] ShutdownPhase phase() const noexcept { return phase_; }

    ShutdownPhase run(const std::function<void()>& cancel,
                      const std::function<bool()>& quiescent,
                      Clock::time_point deadline,
                      std::chrono::milliseconds poll = std::chrono::milliseconds{10}) {
        phase_ = ShutdownPhase::Cancelling;
        cancel();
        phase_ = ShutdownPhase::Draining;
        while (!quiescent()) {
            const auto now = Clock::now();
            if (now >= deadline) {
                phase_ = ShutdownPhase::TimedOut;
                return phase_;
            }
            std::this_thread::sleep_for(std::min(poll,
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        }
        phase_ = ShutdownPhase::Complete;
        return phase_;
    }

private:
    ShutdownPhase phase_{ShutdownPhase::Running};
};

}
