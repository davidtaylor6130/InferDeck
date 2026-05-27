/// @file Logger.cpp
/// @brief Logger implementation using spdlog.

#include "core/Logger.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/pattern_formatter.h>

namespace inferdeck::core {

Logger::Logger()
    : current_level_(LogLevel::Info)
    , log_file_("")
    , console_enabled_(true)
    , initialized_(false) {
}

Logger::~Logger() {
    Shutdown();
}

Logger& Logger::Get() {
    static Logger instance;
    return instance;
}

void Logger::Initialize(LogLevel level, const std::string& log_file, bool console_enabled) {
    if (initialized_) {
        Shutdown();
    }

    current_level_ = level;
    log_file_ = log_file;
    console_enabled_ = console_enabled;

    std::vector<spdlog::sink_ptr> sinks;

    if (console_enabled) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
        sinks.push_back(console_sink);
    }

    if (!log_file.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, true);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("inferdeck", sinks.begin(), sinks.end());

    switch (level) {
        case LogLevel::Trace:  logger->set_level(spdlog::level::trace); break;
        case LogLevel::Debug:  logger->set_level(spdlog::level::debug); break;
        case LogLevel::Info:   logger->set_level(spdlog::level::info); break;
        case LogLevel::Warn:   logger->set_level(spdlog::level::warn); break;
        case LogLevel::Error:  logger->set_level(spdlog::level::err); break;
        case LogLevel::Fatal:  logger->set_level(spdlog::level::critical); break;
    }

    logger->flush_on(spdlog::level::info);

    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);

    initialized_ = true;
}

void Logger::Log(LogLevel level, const std::string& message) {
    if (!initialized_) {
        return;
    }

    auto logger = spdlog::get("inferdeck");
    if (!logger) {
        return;
    }

    switch (level) {
        case LogLevel::Trace:  logger->trace("{}", message); break;
        case LogLevel::Debug:  logger->debug("{}", message); break;
        case LogLevel::Info:   logger->info("{}", message); break;
        case LogLevel::Warn:   logger->warn("{}", message); break;
        case LogLevel::Error:  logger->error("{}", message); break;
        case LogLevel::Fatal:  logger->critical("{}", message); break;
    }
}

void Logger::Info(const std::string& message) {
    Log(LogLevel::Info, message);
}

void Logger::Warn(const std::string& message) {
    Log(LogLevel::Warn, message);
}

void Logger::Error(const std::string& message) {
    Log(LogLevel::Error, message);
}

void Logger::Debug(const std::string& message) {
    Log(LogLevel::Debug, message);
}

void Logger::Trace(const std::string& message) {
    Log(LogLevel::Trace, message);
}

void Logger::Fatal(const std::string& message) {
    Log(LogLevel::Fatal, message);
}

void Logger::SetLevel(LogLevel level) {
    current_level_ = level;
    if (initialized_) {
        auto logger = spdlog::get("inferdeck");
        if (logger) {
            switch (level) {
                case LogLevel::Trace:  logger->set_level(spdlog::level::trace); break;
                case LogLevel::Debug:  logger->set_level(spdlog::level::debug); break;
                case LogLevel::Info:   logger->set_level(spdlog::level::info); break;
                case LogLevel::Warn:   logger->set_level(spdlog::level::warn); break;
                case LogLevel::Error:  logger->set_level(spdlog::level::err); break;
                case LogLevel::Fatal:  logger->set_level(spdlog::level::critical); break;
            }
        }
    }
}

LogLevel Logger::GetLevel() const {
    return current_level_;
}

void Logger::Shutdown() {
    if (initialized_) {
        auto logger = spdlog::get("inferdeck");
        if (logger) {
            logger->flush();
            spdlog::drop("inferdeck");
        }
        initialized_ = false;
    }
}

} // namespace inferdeck::core
