#pragma once

#include <chrono>
#include <string>

namespace inferdeck::apps {

struct SwapReport {
    std::string from_model;
    std::string to_model;
    std::chrono::milliseconds load_ms{0};
    std::chrono::milliseconds unload_ms{0};
    std::chrono::milliseconds total_ms{0};
    bool success{false};
    std::string error{};
};

} // namespace inferdeck::apps
