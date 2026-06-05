#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "foundation/result.hpp"

#ifndef INFERDECK_SOURCE_DIR
#define INFERDECK_SOURCE_DIR "."
#endif

namespace inferdeck::testing {

inline std::filesystem::path fixtures_dir() {
    if (const char* env = std::getenv("INFERDECK_FIXTURES_DIR")) {
        return std::filesystem::path(env);
    }
    return std::filesystem::path(INFERDECK_SOURCE_DIR) / "tests" / "fixtures";
}

inline std::filesystem::path fixture_path(std::string_view name) {
    return fixtures_dir() / name;
}

inline std::optional<std::string> load_fixture_text(std::string_view name) {
    std::ifstream in(fixture_path(name), std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline foundation::Result<nlohmann::json> load_fixture(std::string_view name) {
    auto text = load_fixture_text(name);
    if (!text) {
        return foundation::Err<nlohmann::json>(foundation::ErrorCode::NotFound,
                                                "fixture not found: " + std::string(name));
    }
    try {
        return foundation::Ok<nlohmann::json>(nlohmann::json::parse(*text));
    } catch (const std::exception& e) {
        return foundation::Err<nlohmann::json>(foundation::ErrorCode::ParseError,
                                                std::string("JSON parse: ") + e.what());
    }
}

} // namespace inferdeck::testing

namespace test_helpers = inferdeck::testing;
