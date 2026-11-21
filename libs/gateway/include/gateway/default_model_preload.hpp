#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <thread>
#include <vector>

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

inline std::optional<long long> gpu_backoff_until(const std::vector<long long>& stamps, long long now) {
    long long latest = 0;
    int recent = 0;
    for (const auto stamp : stamps) {
        if (stamp > now + 5) continue;
        latest = std::max(latest, stamp);
        if (now - stamp <= 600) ++recent;
    }
    if (recent >= 2 && now - latest < 1800) return latest + 1800;
    return std::nullopt;
}

}
