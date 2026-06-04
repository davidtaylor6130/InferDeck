#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

namespace inferdeck::foundation {

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
    Off = 6,
};

struct LogConfig {
    LogLevel level{LogLevel::Info};
    std::filesystem::path log_file{};
    bool console_enabled{true};
    std::string pattern{"[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v"};
    bool flush_on_info{true};
};

class Logger {
public:
    static Logger& instance();

    void initialize(const LogConfig& cfg);
    void shutdown();
    void set_level(LogLevel level);

    [[nodiscard]] LogLevel level() const noexcept { return level_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    void log(LogLevel lvl, std::string_view event, std::string_view message);

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex init_mu_;
    LogLevel level_{LogLevel::Info};
    bool initialized_{false};
};

namespace detail {
inline spdlog::level::level_enum to_spdlog(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Trace: return spdlog::level::trace;
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Info:  return spdlog::level::info;
        case LogLevel::Warn:  return spdlog::level::warn;
        case LogLevel::Error: return spdlog::level::err;
        case LogLevel::Fatal: return spdlog::level::critical;
        case LogLevel::Off:   return spdlog::level::off;
    }
    return spdlog::level::info;
}
} // namespace detail

template <typename... Args>
inline void LOG_TRACE(std::string_view event, fmt::format_string<Args...> fmt_str, Args&&... args) {
    Logger::instance().log(LogLevel::Trace, event, fmt::vformat(fmt_str, fmt::make_format_args(args...)));
}

template <typename... Args>
inline void LOG_DEBUG(std::string_view event, fmt::format_string<Args...> fmt_str, Args&&... args) {
    Logger::instance().log(LogLevel::Debug, event, fmt::vformat(fmt_str, fmt::make_format_args(args...)));
}

template <typename... Args>
inline void LOG_INFO(std::string_view event, fmt::format_string<Args...> fmt_str, Args&&... args) {
    Logger::instance().log(LogLevel::Info, event, fmt::vformat(fmt_str, fmt::make_format_args(args...)));
}

template <typename... Args>
inline void LOG_WARN(std::string_view event, fmt::format_string<Args...> fmt_str, Args&&... args) {
    Logger::instance().log(LogLevel::Warn, event, fmt::vformat(fmt_str, fmt::make_format_args(args...)));
}

template <typename... Args>
inline void LOG_ERROR(std::string_view event, fmt::format_string<Args...> fmt_str, Args&&... args) {
    Logger::instance().log(LogLevel::Error, event, fmt::vformat(fmt_str, fmt::make_format_args(args...)));
}

template <typename... Args>
inline void LOG_FATAL(std::string_view event, fmt::format_string<Args...> fmt_str, Args&&... args) {
    Logger::instance().log(LogLevel::Fatal, event, fmt::vformat(fmt_str, fmt::make_format_args(args...)));
}

template <typename... Args>
inline void LOG_INFO_FMT(std::string_view event, fmt::format_string<Args...> fmt_str, Args&&... args) {
    Logger::instance().log(LogLevel::Info, event, fmt::vformat(fmt_str, fmt::make_format_args(args...)));
}

} // namespace inferdeck::foundation
