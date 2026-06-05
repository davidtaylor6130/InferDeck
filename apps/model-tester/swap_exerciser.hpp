#pragma once

#include <chrono>
#include <string>

#include "foundation/result.hpp"
#include "model/backend_coordinator.hpp"
#include "swap_report.hpp"

namespace inferdeck::apps {

using inferdeck::model::BackendCoordinator;

inline SwapReport do_swap(BackendCoordinator& coord,
                          const std::string& from,
                          const std::string& to) {
    SwapReport r;
    r.from_model = from;
    r.to_model = to;
    auto t0 = std::chrono::steady_clock::now();

    auto drain_t0 = std::chrono::steady_clock::now();
    if (!from.empty()) {
        auto r1 = coord.unload_current();
        if (!r1) {
            r.error = "drain: " + r1.error().message;
            r.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0);
            return r;
        }
    }
    auto drain_t1 = std::chrono::steady_clock::now();
    r.unload_ms = std::chrono::duration_cast<std::chrono::milliseconds>(drain_t1 - drain_t0);

    auto load_t0 = std::chrono::steady_clock::now();
    auto r2 = coord.load(to);
    auto load_t1 = std::chrono::steady_clock::now();
    r.load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(load_t1 - load_t0);

    if (!r2) {
        r.error = "load: " + r2.error().message;
        r.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(load_t1 - t0);
        return r;
    }

    r.success = true;
    r.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(load_t1 - t0);
    return r;
}

} // namespace inferdeck::apps
