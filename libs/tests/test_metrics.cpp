/// @file test_metrics.cpp
/// @brief Unit tests for the MetricsStore module.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "core/Metrics.hpp"
#include <nlohmann/json.hpp>

TEST_CASE("MetricsStore singleton returns same instance", "[metrics][singleton]") {
    auto& store1 = inferdeck::core::MetricsStore::Get();
    auto& store2 = inferdeck::core::MetricsStore::Get();
    REQUIRE(&store1 == &store2);
}

TEST_CASE("Counter increments correctly", "[metrics][counter]") {
    auto& store = inferdeck::core::MetricsStore::Get();
    store.Reset();

    store.IncrementCounter("test_counter", 5);
    REQUIRE(store.GetCounter("test_counter") == 5);

    store.IncrementCounter("test_counter", 3);
    REQUIRE(store.GetCounter("test_counter") == 8);
}

TEST_CASE("Gauge sets and retrieves values", "[metrics][gauge]") {
    auto& store = inferdeck::core::MetricsStore::Get();
    store.Reset();

    store.SetGauge("test_gauge", 42.0);
    REQUIRE(store.GetGauge("test_gauge") == Approx(42.0));

    store.SetGauge("test_gauge", 100.0);
    REQUIRE(store.GetGauge("test_gauge") == Approx(100.0));
}

TEST_CASE("Histogram tracks min/max/avg", "[metrics][histogram]") {
    auto& store = inferdeck::core::MetricsStore::Get();
    store.Reset();

    store.RecordHistogram("test_hist", 10.0);
    store.RecordHistogram("test_hist", 20.0);
    store.RecordHistogram("test_hist", 30.0);

    auto hist = store.GetHistogram("test_hist");
    REQUIRE(hist.min_value == Approx(10.0));
    REQUIRE(hist.max_value == Approx(30.0));
    REQUIRE(hist.Average() == Approx(20.0));
    REQUIRE(hist.count == 3);
    REQUIRE(hist.sum == Approx(60.0));
}

TEST_CASE("ToJson returns valid JSON", "[metrics][json]") {
    auto& store = inferdeck::core::MetricsStore::Get();
    store.Reset();

    store.IncrementCounter("req_total", 10);
    store.SetGauge("gpu_mem", 4096.0);
    store.RecordHistogram("latency", 50.0);

    std::string json_str = store.ToJson();
    REQUIRE_NOTHROW(nlohmann::json::parse(json_str));

    nlohmann::json j = nlohmann::json::parse(json_str);
    REQUIRE(j["counters"]["req_total"] == 10);
    REQUIRE(j["gauges"]["gpu_mem"] == 4096.0);
}

TEST_CASE("Reset clears all metrics", "[metrics][reset]") {
    auto& store = inferdeck::core::MetricsStore::Get();

    store.IncrementCounter("test", 100);
    store.SetGauge("test", 99.0);
    store.RecordHistogram("test", 1.0);

    store.Reset();

    REQUIRE(store.GetCounter("test") == 0);
    REQUIRE(store.GetGauge("test") == 0.0);
    auto hist = store.GetHistogram("test");
    REQUIRE(hist.count == 0);
}
