#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "foundation/logging.hpp"

namespace inferdeck::provider {

inline std::optional<foundation::LogLevel> native_log_level(int ggml_level, int& previous) {
    constexpr int warn = 3;
    constexpr int error = 4;
    constexpr int continuation = 5;
    if (ggml_level != continuation) previous = ggml_level;
    if (previous >= error) return foundation::LogLevel::Error;
    if (previous == warn) return foundation::LogLevel::Warn;
    return std::nullopt;
}

inline void forward_native_log(int ggml_level, const char* text) {
    static thread_local int previous = 2;
    const auto level = native_log_level(ggml_level, previous);
    if (!level || !text) return;
    std::string_view line(text);
    while (!line.empty() && (line.back() == 0x0A || line.back() == 0x0D)) line.remove_suffix(1);
    if (!line.empty()) foundation::Logger::instance().log(*level, "native", line);
}

}
