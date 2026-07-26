#include "foundation/logging.hpp"

#include <memory>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace inferdeck::foundation {

Logger& Logger::instance() {
    (void)spdlog::default_logger();
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    shutdown();
}

void Logger::initialize(const LogConfig& cfg) {
    std::vector<spdlog::sink_ptr> sinks;

    if (cfg.console_enabled) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern(cfg.pattern);
        sinks.push_back(console_sink);
    }

    if (!cfg.log_file.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(cfg.log_file.string(), false);
        file_sink->set_pattern(cfg.pattern);
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("inferdeck-foundation", sinks.begin(), sinks.end());
    logger->set_level(detail::to_spdlog(cfg.level));
    if (cfg.flush_on_info) {
        logger->flush_on(spdlog::level::info);
    }

    std::lock_guard<std::mutex> lock(state_mu_);
    if (logger_) {
        initialized_.store(false, std::memory_order_release);
        logger_->flush();
        spdlog::drop(logger_->name());
        logger_.reset();
    } else {
        previous_default_logger_ = spdlog::default_logger();
    }

    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);
    logger_ = logger;
    level_.store(cfg.level, std::memory_order_release);
    initialized_.store(true, std::memory_order_release);
    logger_->info("event=logger_init level={} console={} file={}",
                  static_cast<int>(cfg.level),
                  cfg.console_enabled,
                  cfg.log_file.string());
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(state_mu_);
    initialized_.store(false, std::memory_order_release);
    if (logger_) {
        logger_->flush();
        if (spdlog::default_logger() == logger_) {
            spdlog::set_default_logger(previous_default_logger_);
        }
        spdlog::drop(logger_->name());
        logger_.reset();
    }
    previous_default_logger_.reset();
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(state_mu_);
    level_.store(level, std::memory_order_release);
    if (logger_) {
        logger_->set_level(detail::to_spdlog(level));
    }
}

void Logger::log(LogLevel lvl, std::string_view event, std::string_view message) {
    std::lock_guard<std::mutex> lock(state_mu_);
    if (!logger_) {
        return;
    }
    const std::string line = fmt::format("event={} {}", event, message);
    switch (lvl) {
        case LogLevel::Trace: logger_->trace("{}", line); break;
        case LogLevel::Debug: logger_->debug("{}", line); break;
        case LogLevel::Info:  logger_->info("{}", line); break;
        case LogLevel::Warn:  logger_->warn("{}", line); break;
        case LogLevel::Error: logger_->error("{}", line); break;
        case LogLevel::Fatal: logger_->critical("{}", line); break;
        case LogLevel::Off: break;
    }
}

} // namespace inferdeck::foundation
