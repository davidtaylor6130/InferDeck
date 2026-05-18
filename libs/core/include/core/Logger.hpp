/// @file Logger.hpp
/// @brief Logging interface wrapping spdlog for InferDeck.
///
/// Provides a singleton logger with configurable levels, file output,
/// and console output. All public APIs use Doxygen XML-style comments.

#pragma once

#include <string>
#include <filesystem>
#include <memory>

namespace inferdeck::core {

/// Log levels supported by the logger.
enum class LogLevel {
    Trace,  ///< Most verbose, detailed debug information
    Debug,  ///< Debugging information
    Info,   ///< General informational messages
    Warn,   ///< Warning messages
    Error,  ///< Error messages
    Fatal   ///< Fatal error messages
};

/// Logger singleton providing centralized logging for the application.
///
/// This class wraps spdlog and provides a simple interface for logging
/// messages at different levels. It supports both console and file output.
class Logger {
public:
    /// Get the singleton Logger instance.
    /// @return Reference to the singleton Logger instance.
    static Logger& Get();

    /// Initialize the logger with configuration.
    /// @param level The minimum log level to output.
    /// @param log_file Path to the log file. Empty string disables file logging.
    /// @param console_enabled Whether to enable console output.
    void Initialize(LogLevel level, const std::string& log_file, bool console_enabled = true);

    /// Log a message at the specified level.
    /// @param level The log level for this message.
    /// @param message The message to log.
    void Log(LogLevel level, const std::string& message);

    /// Log an informational message.
    /// @param message The message to log.
    void Info(const std::string& message);

    /// Log a warning message.
    /// @param message The message to log.
    void Warn(const std::string& message);

    /// Log an error message.
    /// @param message The message to log.
    void Error(const std::string& message);

    /// Log a debug message.
    /// @param message The message to log.
    void Debug(const std::string& message);

    /// Log a trace message.
    /// @param message The message to log.
    void Trace(const std::string& message);

    /// Log a fatal message.
    /// @param message The message to log.
    void Fatal(const std::string& message);

    /// Set the log level.
    /// @param level The new log level.
    void SetLevel(LogLevel level);

    /// Get the current log level.
    /// @return The current log level.
    LogLevel GetLevel() const;

    /// Shutdown the logger and release resources.
    void Shutdown();

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel current_level_;
    std::string log_file_;
    bool console_enabled_;
    bool initialized_;
};

} // namespace inferdeck::core
