#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace inferdeck::gateway {

inline bool valid_request_id(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    for (unsigned char c : value) {
        const bool alphanumeric = (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alphanumeric && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
}

inline std::string request_id(std::string_view supplied) {
    if (valid_request_id(supplied)) return std::string{supplied};
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream output;
    output << "req_" << std::hex << timestamp << '_'
           << sequence.fetch_add(1, std::memory_order_relaxed);
    return output.str();
}

}
