#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>

#include "gateway/config_repository.hpp"

using inferdeck::foundation::ErrorCode;
using inferdeck::foundation::Ok;
using inferdeck::gateway::ConfigRepository;

namespace {

struct ConfigFiles {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("inferdeck-config-repository-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path base = root / "gateway.yml";
    std::filesystem::path active = root / "gateway.active.yml";

    ConfigFiles() {
        std::filesystem::create_directories(root);
        write(base, "schema_version: 1\nvalue: base\n");
    }

    ~ConfigFiles() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    static void write(const std::filesystem::path& path,
                      const std::string& text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
    }

    static std::string read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
    }
};

}

TEST_CASE("ConfigRepository serializes optimistic concurrent writes",
          "[config][repository][concurrency]") {
    ConfigFiles files;
    ConfigRepository repository(files.base, files.active,
        [](const std::string&) { return Ok(); }, [] { return Ok(); });
    const auto initial = repository.snapshot();
    REQUIRE(initial);

    auto first = std::async(std::launch::async, [&] {
        return repository.write_active(
            initial->active_revision, "schema_version: 1\nvalue: first\n");
    });
    auto second = std::async(std::launch::async, [&] {
        return repository.write_active(
            initial->active_revision, "schema_version: 1\nvalue: second\n");
    });
    const auto first_result = first.get();
    const auto second_result = second.get();

    REQUIRE(first_result.has_value() != second_result.has_value());
    const auto& conflict = first_result ? second_result : first_result;
    REQUIRE(conflict.error().code == ErrorCode::AlreadyExists);
    const auto saved = ConfigFiles::read(files.active);
    REQUIRE((saved == "schema_version: 1\nvalue: first\n" ||
             saved == "schema_version: 1\nvalue: second\n"));
}

TEST_CASE("ConfigRepository restores active file when reload fails",
          "[config][repository][rollback]") {
    ConfigFiles files;
    ConfigFiles::write(files.active, "schema_version: 1\nvalue: previous\n");
    ConfigRepository repository(files.base, files.active,
        [](const std::string&) { return Ok(); }, [] {
            return inferdeck::foundation::Err<void>(
                ErrorCode::Unavailable, "reload rejected");
        });
    const auto initial = repository.snapshot();
    REQUIRE(initial);

    const auto result = repository.write_active(
        initial->active_revision, "schema_version: 1\nvalue: replacement\n");

    REQUIRE_FALSE(result);
    REQUIRE(result.error().message.find("restored") != std::string::npos);
    REQUIRE(ConfigFiles::read(files.active) == initial->active);
}

TEST_CASE("ConfigRepository reset checks revision and rolls back reload failure",
          "[config][repository][conflict][rollback]") {
    ConfigFiles files;
    ConfigFiles::write(files.active, "schema_version: 1\nvalue: active\n");
    ConfigRepository repository(files.base, files.active,
        [](const std::string&) { return Ok(); }, [] {
            return inferdeck::foundation::Err<void>(
                ErrorCode::Unavailable, "reload rejected");
        });
    const auto initial = repository.snapshot();
    REQUIRE(initial);

    const auto conflict = repository.reset_active("stale");
    REQUIRE_FALSE(conflict);
    REQUIRE(conflict.error().code == ErrorCode::AlreadyExists);
    REQUIRE(ConfigFiles::read(files.active) == initial->active);

    const auto failed_reload = repository.reset_active(initial->active_revision);
    REQUIRE_FALSE(failed_reload);
    REQUIRE(ConfigFiles::read(files.active) == initial->active);
}
