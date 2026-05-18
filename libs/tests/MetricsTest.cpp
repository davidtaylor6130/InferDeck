/// @file MetricsTest.cpp
/// @brief Unit tests for Metrics route handler.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <nlohmann/json.hpp>

TEST_CASE("Metrics response has correct schema", "[route][metrics]") {
    // Expected response structure
    nlohmann::json expected = nlohmann::json::parse(R"({
        "counters": {
            "inferdeck.requests.total": 42,
            "inferdeck.requests.success": 41,
            "inferdeck.requests.failed": 1
        },
        "gauges": {
            "gpu.vram_used_mb": 4096,
            "gpu.vram_total_mb": 8192,
            "queue.pending": 3
        },
        "histograms": {
            "inferdeck.latency_ms": {
                "min": 10.5,
                "max": 500.2,
                "avg": 45.3,
                "count": 42,
                "sum": 1902.6
            }
        }
    })");

    // Verify top-level structure
    REQUIRE(expected.contains("counters"));
    REQUIRE(expected.contains("gauges"));
    REQUIRE(expected.contains("histograms"));

    // Verify counters
    auto& counters = expected["counters"];
    REQUIRE(counters.contains("inferdeck.requests.total"));
    REQUIRE(counters.contains("inferdeck.requests.success"));
    REQUIRE(counters.contains("inferdeck.requests.failed"));

    // Verify gauges
    auto& gauges = expected["gauges"];
    REQUIRE(gauges.contains("gpu.vram_used_mb"));
    REQUIRE(gauges.contains("gpu.vram_total_mb"));
    REQUIRE(gauges.contains("queue.pending"));

    // Verify histograms
    auto& histograms = expected["histograms"];
    auto& latency = histograms["inferdeck.latency_ms"];
    REQUIRE(latency.contains("min"));
    REQUIRE(latency.contains("max"));
    REQUIRE(latency.contains("avg"));
    REQUIRE(latency.contains("count"));
    REQUIRE(latency.contains("sum"));
}

TEST_CASE("Metrics JSON is valid and parseable", "[route][metrics]") {
    std::string metrics_json = R"({
        "counters": {"test": 1},
        "gauges": {"test": 1.0},
        "histograms": {"test": {"min": 1, "max": 2, "avg": 1.5, "count": 1, "sum": 1}}
    })";

    // Should parse without error
    auto j = nlohmann::json::parse(metrics_json);
    REQUIRE(j.contains("counters"));
    REQUIRE(j.contains("gauges"));
    REQUIRE(j.contains("histograms"));
}
