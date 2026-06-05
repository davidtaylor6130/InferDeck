#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace inferdeck::observability {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

struct LogEvent {
  std::string event;
  std::string message;
  std::int64_t timestamp_unix_ms{};
  LogLevel level{LogLevel::Info};
};

class Logger {
public:
  Logger(LogLevel min_level, std::string sink_path = {});
  ~Logger();

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  bool enabled(LogLevel level) const noexcept {
    return static_cast<int>(level) >= static_cast<int>(min_level_);
  }

  void log(LogLevel level, std::string_view event, std::string_view message);

  void trace(std::string_view e, std::string_view m) { log(LogLevel::Trace, e, m); }
  void debug(std::string_view e, std::string_view m) { log(LogLevel::Debug, e, m); }
  void info(std::string_view e, std::string_view m)  { log(LogLevel::Info,  e, m); }
  void warn(std::string_view e, std::string_view m)  { log(LogLevel::Warn,  e, m); }
  void error(std::string_view e, std::string_view m) { log(LogLevel::Error, e, m); }

  LogLevel min_level() const noexcept { return min_level_; }

  std::size_t events_logged() const {
    std::lock_guard lk(mtx_);
    return events_logged_;
  }

  static LogLevel parse_level(std::string_view text);
  static std::string level_name(LogLevel level);

private:
  LogLevel min_level_;
  std::string sink_path_;
  mutable std::mutex mtx_;
  std::size_t events_logged_{};
};

}
