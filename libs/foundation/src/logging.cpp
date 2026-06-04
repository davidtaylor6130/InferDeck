#include "foundation/logging.hpp"

#include <memory>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace inferdeck::foundation {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    shutdown();
}

void Logger::initialize(const LogConfig& cfg) {
    std::lock_guard<std::mutex> lock(init_mu_);
    if (initialized_) {
        shutdown();
    }

    level_ = cfg.level;
    std::vector<spdlog::sink_ptr> sinks;

    if (cfg.console_enabled) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern(cfg.pattern);
        sinks.push_back(console_sink);
    }

    if (!cfg.log_file.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(cfg.log_file.string(), true);
        file_sink->set_pattern(cfg.pattern);
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("inferdeck-foundation", sinks.begin(), sinks.end());
    logger->set_level(detail::to_spdlog(cfg.level));
    if (cfg.flush_on_info) {
        logger->flush_on(spdlog::level::info);
    }

    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);
    initialized_ = true;

    log(LogLevel::Info, "logger_init", fmt::format("level={} console={} file={}",
                                                  static_cast<int>(cfg.level),
                                                  cfg.console_enabled,
                                                  cfg.log_file.string()));
}

void Logger::shutdown() {
    if (initialized_) {
        if (auto logger = spdlog::get("inferdeck-foundation"); logger) {
            logger->flush();
            spdlog::drop("inferdeck-foundation");
        }
        initialized_ = false;
    }
}

void Logger::set_level(LogLevel level) {
    level_ = level;
    if (initialized_) {
        if (auto logger = spdlog::get("inferdeck-foundation"); logger) {
            logger->set_level(detail::to_spdlog(level));
        }
    }
}

void Logger::log(LogLevel lvl, std::string_view event, std::string_view message) {
    if (!initialized_) {
        return;
    }
    auto logger = spdlog::get("inferdeck-foundation");
    if (!logger) {
        return;
    }
    const std::string line = fmt::format("event={} {}", event, message);
    switch (lvl) {
        case LogLevel::Trace: logger->trace("{}", line); break;
        case LogLevel::Debug: logger->debug("{}", line); break;
        case LogLevel::Info:  logger->info("{}", line); break;
        case LogLevel::Warn:  logger->warn("{}", line); break;
        case LogLevel::Error: logger->error("{}", line); break;
        case LogLevel::Fatal: logger->critical("{}", line); break;
        case LogLevel::Off: break;
    }
}

} // namespace inferdeck::foundation
