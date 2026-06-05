#pragma once

#include <iostream>
#include <string>

#include "swap_report.hpp"

namespace inferdeck::apps {

inline void print_report(const SwapReport& r) {
    std::cout << "[model-tester] ";
    if (r.success) {
        std::cout << "swap ok: " << r.from_model << " -> " << r.to_model
                  << " (unload=" << r.unload_ms.count() << "ms, "
                  << "load=" << r.load_ms.count() << "ms, "
                  << "total=" << r.total_ms.count() << "ms)";
    } else {
        std::cout << "swap FAILED: " << r.from_model << " -> " << r.to_model
                  << " error: " << r.error;
    }
    std::cout << std::endl;
}

} // namespace inferdeck::apps
