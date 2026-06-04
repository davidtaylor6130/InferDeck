#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#include <process.h>
#define getpid() _getpid()

#include "foundation/async.hpp"
#include "foundation/json_utils.hpp"
#include "foundation/logging.hpp"
#include "foundation/result.hpp"

using namespace inferdeck::foundation;

TEST_CASE("Result Ok and Err", "[result]") {
    auto ok_int = Ok<int>(42);
    REQUIRE(ok_int.has_value());
    REQUIRE(ok_int.value() == 42);

    auto err = Err<int>(ErrorCode::NotFound, "missing");
    REQUIRE_FALSE(err.has_value());
    REQUIRE(err.error().code == ErrorCode::NotFound);
    REQUIRE(err.error().message == "missing");
}

TEST_CASE("Result void Ok", "[result]") {
    auto ok = Ok();
    REQUIRE(ok.has_value());
}

TEST_CASE("Logger initialization and event log", "[logging]") {
    auto log_path = std::filesystem::temp_directory_path() /
                    ("inferdeck_foundation_test_" + std::to_string(::getpid()) + ".log");

    {
        LogConfig cfg;
        cfg.level = LogLevel::Debug;
        cfg.console_enabled = false;
        cfg.log_file = log_path;
        std::error_code ec;
        std::filesystem::remove(log_path, ec);

        Logger::instance().initialize(cfg);
        REQUIRE(Logger::instance().initialized());

        LOG_INFO("test_event", "value={} name={}", 7, std::string("hello"));
        LOG_DEBUG("debug_event", "answer={}", 42);
        LOG_ERROR("error_event", "code={} reason={}", 1, std::string("boom"));

        Logger::instance().shutdown();
    }

    std::ifstream in(log_path);
    REQUIRE(in.is_open());
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string contents = ss.str();
    REQUIRE(contents.find("test_event") != std::string::npos);
    REQUIRE(contents.find("value=7") != std::string::npos);
    REQUIRE(contents.find("name=hello") != std::string::npos);
    REQUIRE(contents.find("debug_event") != std::string::npos);
    REQUIRE(contents.find("error_event") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(log_path, ec);
}

TEST_CASE("JSON parse and round-trip", "[json]") {
    auto parsed = parse_json(R"({"name":"inferdeck","v":1})");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value()["name"] == "inferdeck");
    REQUIRE(parsed.value()["v"] == 1);

    auto dump = dump_json(parsed.value(), false);
    REQUIRE(dump.find("\"name\":\"inferdeck\"") != std::string::npos);

    auto bad = parse_json("{not json");
    REQUIRE_FALSE(bad.has_value());
    REQUIRE(bad.error().code == ErrorCode::ParseError);
}

TEST_CASE("JSON file save/load", "[json]") {
    auto path = std::filesystem::temp_directory_path() /
                ("inferdeck_foundation_test_" + std::to_string(::getpid()) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    Json v = {{"a", 1}, {"b", {"x", "y", "z"}}, {"c", true}};
    auto saved = save_json_file(path, v, true);
    REQUIRE(saved.has_value());
    REQUIRE(std::filesystem::exists(path));

    auto loaded = load_json_file(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value()["a"] == 1);
    REQUIRE(loaded.value()["b"].size() == 3);
    REQUIRE(loaded.value()["c"] == true);

    std::filesystem::remove(path, ec);
}

TEST_CASE("hello_future: async execution", "[async]") {
    auto fut = run_async([]() -> int {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 7 * 6;
    });

    REQUIRE(fut.valid());
    auto status = fut.wait_for(std::chrono::seconds(2));
    REQUIRE(status == std::future_status::ready);
    REQUIRE(fut.get() == 42);
}

TEST_CASE("hello_future: async exception propagation", "[async]") {
    auto fut = run_async([]() -> int {
        throw std::runtime_error("async failure");
    });
    REQUIRE(fut.valid());
    REQUIRE_THROWS_AS(fut.get(), std::runtime_error);
}

TEST_CASE("StopWatch measures elapsed time", "[async]") {
    StopWatch sw;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(sw.elapsed_ms() >= 5.0);
    sw.reset();
    REQUIRE(sw.elapsed_ms() < 100.0);
}
