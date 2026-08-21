#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>

#include "gateway/shutdown_state.hpp"

using namespace inferdeck::gateway;

TEST_CASE("Shutdown state completes after cancellation drains work",
          "[shutdown][lifecycle]") {
    ShutdownStateMachine shutdown;
    std::atomic<bool> cancelled{false};
    const auto result = shutdown.run(
        [&] { cancelled.store(true); },
        [&] { return cancelled.load(); },
        ShutdownStateMachine::Clock::now() + std::chrono::milliseconds{100});

    CHECK(result == ShutdownPhase::Complete);
    CHECK(shutdown.phase() == ShutdownPhase::Complete);
}

TEST_CASE("Shutdown state bounds a backend that never returns",
          "[shutdown][lifecycle][deadline]") {
    ShutdownStateMachine shutdown;
    std::atomic<bool> cancellation_requested{false};
    const auto started = ShutdownStateMachine::Clock::now();
    const auto result = shutdown.run(
        [&] { cancellation_requested.store(true); },
        [] { return false; },
        started + std::chrono::milliseconds{35},
        std::chrono::milliseconds{1});
    const auto elapsed = ShutdownStateMachine::Clock::now() - started;

    CHECK(cancellation_requested.load());
    CHECK(result == ShutdownPhase::TimedOut);
    CHECK(elapsed < std::chrono::milliseconds{150});
}
