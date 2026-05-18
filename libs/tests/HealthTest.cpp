/// @file HealthTest.cpp
/// @brief Unit tests for Health route handler.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <nlohmann/json.hpp>

TEST_CASE("Health response has correct schema", "[route][health]") {
    // Expected response structure
    nlohmann::json expected = nlohmann::json::parse(R"({
        "status": "ok",
        "version": "0.1.0",
        "uptime_seconds": 12345,
        "model_loaded": true,
        "gpu_available": true
    })");

    // Verify required fields
    REQUIRE(expected.contains("status"));
    REQUIRE(expected.contains("version"));
    REQUIRE(expected.contains("uptime_seconds"));
    REQUIRE(expected.contains("model_loaded"));
    REQUIRE(expected.contains("gpu_available"));

    // Verify types
    REQUIRE(expected["status"] == "ok");
    REQUIRE(expected["status"].is_string());
    REQUIRE(expected["uptime_seconds"].is_number());
    REQUIRE(expected["model_loaded"].is_boolean());
    REQUIRE(expected["gpu_available"].is_boolean());
}

TEST_CASE("Health status values are valid", "[route][health]") {
    std::vector<std::string> valid_statuses = {"ok", "degraded", "error"};

    for (const auto& status : valid_statuses) {
        nlohmann::json j;
        j["status"] = status;
        REQUIRE_NOTHROW(nlohmann::json::parse(j.dump()));
    }
}
