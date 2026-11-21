#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

namespace inferdeck::gateway {

inline bool wait_for_default_preload(std::chrono::milliseconds delay,
                                     const std::function<bool()>& aborted,
                                     const std::function<bool()>& already_resident) {
    const auto until = std::chrono::steady_clock::now() + delay;
    while (std::chrono::steady_clock::now() < until && !aborted()) {
        std::this_thread::sleep_for(std::min(std::chrono::milliseconds{50}, delay));
    }
    return !aborted() && !already_resident();
}

}
