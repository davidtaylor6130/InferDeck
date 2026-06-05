#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "observability/logger.hpp"

using namespace inferdeck::observability;

TEST_CASE("Logger parse_level", "[observability][logger]") {
  REQUIRE(Logger::parse_level("trace") == LogLevel::Trace);
  REQUIRE(Logger::parse_level("DEBUG") == LogLevel::Debug);
  REQUIRE(Logger::parse_level("info")  == LogLevel::Info);
  REQUIRE(Logger::parse_level("WARN")  == LogLevel::Warn);
  REQUIRE(Logger::parse_level("warning") == LogLevel::Warn);
  REQUIRE(Logger::parse_level("error") == LogLevel::Error);
  REQUIRE(Logger::parse_level("err")   == LogLevel::Error);
  REQUIRE(Logger::parse_level("nonsense") == LogLevel::Info);
  REQUIRE(Logger::parse_level("") == LogLevel::Info);
}

TEST_CASE("Logger level_name round-trip", "[observability][logger]") {
  REQUIRE(Logger::level_name(LogLevel::Trace) == "trace");
  REQUIRE(Logger::level_name(LogLevel::Debug) == "debug");
  REQUIRE(Logger::level_name(LogLevel::Info)  == "info");
  REQUIRE(Logger::level_name(LogLevel::Warn)  == "warn");
  REQUIRE(Logger::level_name(LogLevel::Error) == "error");
}

TEST_CASE("Logger filters below min_level", "[observability][logger]") {
  Logger log(LogLevel::Warn);
  log.debug("d", "should_drop");
  log.info("i",  "should_drop");
  log.warn("w",  "should_keep");
  log.error("e", "should_keep");
  REQUIRE(log.events_logged() == 2);
}

TEST_CASE("Logger writes to sink file", "[observability][logger]") {
  static std::atomic<int> counter{0};
  const int id = counter.fetch_add(1);
  const auto sink = std::filesystem::temp_directory_path() /
                    ("inferdeck_log_test_" + std::to_string(id) + ".log");
  std::error_code ec;
  std::filesystem::remove(sink, ec);
  {
    Logger log(LogLevel::Info, sink.string());
    log.info("model_loaded", "name=qwen3");
    log.info("swap_started", "from=a to=b");
  }
  REQUIRE(std::filesystem::exists(sink));
  std::ifstream in(sink);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  REQUIRE(content.find("\"event\":\"model_loaded\"") != std::string::npos);
  REQUIRE(content.find("\"message\":\"name=qwen3\"") != std::string::npos);
  REQUIRE(content.find("\"event\":\"swap_started\"") != std::string::npos);
  REQUIRE(content.find("\"level\":\"info\"") != std::string::npos);
}

TEST_CASE("Logger escapes control chars and quotes", "[observability][logger]") {
  static std::atomic<int> counter{0};
  const int id = counter.fetch_add(1);
  const auto sink = std::filesystem::temp_directory_path() /
                    ("inferdeck_log_esc_" + std::to_string(id) + ".log");
  std::error_code ec;
  std::filesystem::remove(sink, ec);
  {
    Logger log(LogLevel::Info, sink.string());
    log.info("k", "line1\nline2\twith \"quote\" and \\ slash");
  }
  std::ifstream in(sink);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  REQUIRE(content.find("\\n") != std::string::npos);
  REQUIRE(content.find("\\t") != std::string::npos);
  REQUIRE(content.find("\\\"quote\\\"") != std::string::npos);
  REQUIRE(content.find("\\\\") != std::string::npos);
}

// sentinel

