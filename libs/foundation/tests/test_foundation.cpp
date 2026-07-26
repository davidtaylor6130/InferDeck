#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <process.h>
#define getpid() _getpid()

#include "foundation/async.hpp"
#include "foundation/event_bus.hpp"
#include "foundation/json_utils.hpp"
#include "foundation/logging.hpp"
#include "foundation/path_utils.hpp"
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

TEST_CASE("Path containment compares components", "[foundation][path]") {
    const std::filesystem::path root = "/srv/inferdeck/static";
    CHECK(is_path_within(root, root));
    CHECK(is_path_within(root, root / "assets" / "index.js"));
    CHECK_FALSE(is_path_within(root, "/srv/inferdeck/static-old/index.js"));
    CHECK_FALSE(is_path_within(root, root / ".." / "config" / "gateway.yml"));
    CHECK_FALSE(is_path_within(root, "/other/root/index.js"));
}

TEST_CASE("User paths expand only a leading home component", "[foundation][path]") {
    const auto expanded_home = expand_user_path("~");
    const auto expanded_file = expand_user_path(
        std::filesystem::path("~") / "InferDeck" / L"díctation.json");
    if (expanded_home != "~") {
        CHECK(expanded_file == expanded_home / "InferDeck" / L"díctation.json");
    } else {
        CHECK(expanded_file == std::filesystem::path("~") /
                                   "InferDeck" / L"díctation.json");
    }
    CHECK(expand_user_path("~another-user/state.json") ==
          std::filesystem::path("~another-user/state.json"));
    CHECK(expand_user_path({}).empty());
}

TEST_CASE("Path containment accepts weakly canonical descendants", "[foundation][path]") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("inferdeck_foundation_path_" +
                            std::to_string(::getpid()));
    const auto root_path = directory / L"Mødel Store";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    REQUIRE(std::filesystem::create_directories(root_path / "nested", error));

    const auto root = std::filesystem::weakly_canonical(root_path, error);
    REQUIRE_FALSE(error);
    const auto inside = std::filesystem::weakly_canonical(
        root_path / "nested" / ".." / "state.json", error);
    REQUIRE_FALSE(error);
    const auto outside = std::filesystem::weakly_canonical(
        root_path / ".." / "gateway.yml", error);
    REQUIRE_FALSE(error);

    CHECK(is_path_within(root, inside));
    CHECK_FALSE(is_path_within(root, outside));
    std::filesystem::remove_all(directory, error);
}

#ifdef _WIN32
TEST_CASE("Path containment follows Windows case and traversal semantics", "[foundation][path]") {
    const std::filesystem::path root = LR"(C:\InferDeck\Mødel Store)";
    CHECK(is_path_within(root, LR"(c:\inferdeck\Mødel Store\Qwen\model.gguf)"));
    CHECK(is_path_within(root, LR"(C:\InferDeck\Mødel Store)"));
    CHECK_FALSE(is_path_within(root, LR"(C:\InferDeck\Mødel Store-old\model.gguf)"));
    CHECK_FALSE(is_path_within(root, LR"(C:\InferDeck\Mødel Store\..\gateway.yml)"));
    CHECK_FALSE(is_path_within(root, LR"(D:\InferDeck\Mødel Store\model.gguf)"));
}
#endif

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
        Logger::instance().initialize(cfg);
        LOG_INFO("post_reload_event", "profile=active");
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
    REQUIRE(contents.find("post_reload_event") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(log_path, ec);
}

TEST_CASE("Logger lifecycle is safe during concurrent logging", "[logging]") {
    const auto log_path = std::filesystem::temp_directory_path() /
                          ("inferdeck_foundation_concurrent_" +
                           std::to_string(::getpid()) + ".log");
    std::error_code ec;
    std::filesystem::remove(log_path, ec);

    LogConfig cfg;
    cfg.level = LogLevel::Debug;
    cfg.console_enabled = false;
    cfg.log_file = log_path;
    Logger::instance().initialize(cfg);

    std::atomic_bool stop{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([worker, &stop] {
            int message = 0;
            while (!stop.load(std::memory_order_acquire)) {
                LOG_INFO("concurrent_log", "worker={} message={}", worker, message++);
            }
        });
    }

    for (int cycle = 0; cycle < 10; ++cycle) {
        Logger::instance().set_level(cycle % 2 == 0 ? LogLevel::Debug : LogLevel::Info);
        Logger::instance().shutdown();
        Logger::instance().initialize(cfg);
    }
    stop.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    Logger::instance().shutdown();

    std::ifstream input(log_path);
    REQUIRE(input.is_open());
    std::ostringstream contents;
    contents << input.rdbuf();
    CHECK(contents.str().find("logger_init") != std::string::npos);
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

TEST_CASE("JSON file replacement preserves the previous file on serialization failure", "[json]") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("inferdeck_foundation_json_" +
                            std::to_string(::getpid()));
    const auto path = directory / "state.json";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    REQUIRE(std::filesystem::create_directories(directory, ec));

    REQUIRE(save_json_file(path, Json{{"generation", 1}}, false));
    Json invalid = std::string(1, static_cast<char>(0xff));
    const auto failed = save_json_file(path, invalid, false);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == ErrorCode::InvalidArgument);

    const auto loaded = load_json_file(path);
    REQUIRE(loaded);
    CHECK(loaded->at("generation") == 1);
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        CHECK(entry.path().filename().string().find(".tmp.") == std::string::npos);
    }
    std::filesystem::remove_all(directory, ec);
}

TEST_CASE("Concurrent JSON saves leave one complete document", "[json]") {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("inferdeck_foundation_json_concurrent_" +
                            std::to_string(::getpid()));
    const auto path = directory / "state.json";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    REQUIRE(std::filesystem::create_directories(directory, ec));

    std::vector<std::future<Result<void>>> saves;
    for (int generation = 0; generation < 12; ++generation) {
        saves.push_back(run_async([path, generation] {
            return save_json_file(path,
                                  Json{{"generation", generation},
                                       {"payload", std::string(4096, 'x')}},
                                  false);
        }));
    }
    for (auto& save : saves) {
        const auto result = save.get();
        const auto message = result.has_value() ? std::string{} : result.error().message;
        INFO(message);
        REQUIRE(result);
    }

    const auto loaded = load_json_file(path);
    REQUIRE(loaded);
    CHECK(loaded->at("generation").get<int>() >= 0);
    CHECK(loaded->at("generation").get<int>() < 12);
    CHECK(loaded->at("payload").get<std::string>().size() == 4096);
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        CHECK(entry.path().filename().string().find(".tmp.") == std::string::npos);
    }
    std::filesystem::remove_all(directory, ec);
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

TEST_CASE("run_async future owns unfinished work", "[async]") {
    std::atomic_bool finished{false};
    {
        auto future = run_async([&finished] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            finished.store(true, std::memory_order_release);
        });
        REQUIRE(future.valid());
    }
    CHECK(finished.load(std::memory_order_acquire));
}

TEST_CASE("StopWatch measures elapsed time", "[async]") {
    StopWatch sw;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(sw.elapsed_ms() >= 5.0);
    sw.reset();
    REQUIRE(sw.elapsed_ms() < 100.0);
}

TEST_CASE("EventBus zero queue request remains bounded and usable", "[event_bus]") {
    EventBus bus;
    auto subscription = bus.subscribe(0);
    bus.publish("first", "1");
    bus.publish("second", "2");
    const auto event = subscription->wait_for(std::chrono::milliseconds{10});
    REQUIRE(event);
    CHECK(event->name == "second");
    CHECK(event->data == "2");
}
