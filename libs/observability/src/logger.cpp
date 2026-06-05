#include "observability/logger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>

namespace inferdeck::observability {

namespace {

std::int64_t now_unix_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string escape_field(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}

Logger::Logger(LogLevel min_level, std::string sink_path)
    : min_level_(min_level), sink_path_(std::move(sink_path)) {}

Logger::~Logger() = default;

void Logger::log(LogLevel level, std::string_view event, std::string_view message) {
  if (!enabled(level)) return;
  LogEvent ev{
    std::string(event),
    std::string(message),
    now_unix_ms(),
    level
  };
  std::string line = "{"
    "\"ts\":" + std::to_string(ev.timestamp_unix_ms) + ","
    "\"level\":\"" + level_name(ev.level) + "\","
    "\"event\":\"" + escape_field(ev.event) + "\","
    "\"message\":\"" + escape_field(ev.message) + "\""
    "}\n";

  std::lock_guard lk(mtx_);
  ++events_logged_;
  if (!sink_path_.empty()) {
    std::ofstream out(sink_path_, std::ios::app);
    if (out) out << line;
  } else {
    std::cout << line;
    std::cout.flush();
  }
}

LogLevel Logger::parse_level(std::string_view text) {
  std::string lower;
  lower.reserve(text.size());
  for (const char c : text) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (lower == "trace") return LogLevel::Trace;
  if (lower == "debug") return LogLevel::Debug;
  if (lower == "warn" || lower == "warning") return LogLevel::Warn;
  if (lower == "error" || lower == "err") return LogLevel::Error;
  return LogLevel::Info;
}

std::string Logger::level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info:  return "info";
    case LogLevel::Warn:  return "warn";
    case LogLevel::Error: return "error";
  }
  return "info";
}

}
