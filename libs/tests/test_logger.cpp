/// @file test_logger.cpp
/// @brief Unit tests for the Logger module.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "core/Logger.hpp"
#include <filesystem>

TEST_CASE("Logger singleton returns same instance", "[logger][singleton]") {
    auto& logger1 = inferdeck::core::Logger::Get();
    auto& logger2 = inferdeck::core::Logger::Get();
    REQUIRE(&logger1 == &logger2);
}

TEST_CASE("Logger initializes with config", "[logger][init]") {
    auto& logger = inferdeck::core::Logger::Get();

    std::filesystem::path log_file = std::filesystem::temp_directory_path() / "test_logger.log";

    logger.Initialize(inferdeck::core::LogLevel::Debug, log_file.string(), true);
    REQUIRE(logger.GetLevel() == inferdeck::core::LogLevel::Debug);

    logger.Shutdown();
    std::filesystem::remove(log_file);
}

TEST_CASE("Logger sets correct log level", "[logger][level]") {
    auto& logger = inferdeck::core::Logger::Get();

    logger.Initialize(inferdeck::core::LogLevel::Info, "", false);
    REQUIRE(logger.GetLevel() == inferdeck::core::LogLevel::Info);

    logger.SetLevel(inferdeck::core::LogLevel::Error);
    REQUIRE(logger.GetLevel() == inferdeck::core::LogLevel::Error);

    logger.Shutdown();
}

TEST_CASE("Logger logs at all levels", "[logger][log]") {
    auto& logger = inferdeck::core::Logger::Get();

    logger.Initialize(inferdeck::core::LogLevel::Trace, "", false);

    REQUIRE_NOTHROW(logger.Trace("trace message"));
    REQUIRE_NOTHROW(logger.Debug("debug message"));
    REQUIRE_NOTHROW(logger.Info("info message"));
    REQUIRE_NOTHROW(logger.Warn("warn message"));
    REQUIRE_NOTHROW(logger.Error("error message"));
    REQUIRE_NOTHROW(logger.Fatal("fatal message"));

    logger.Shutdown();
}
