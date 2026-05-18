/// @file Metrics.hpp
/// @brief In-memory metrics store for InferDeck.
///
/// Provides counters, gauges, and histograms for monitoring the gateway
/// service. Metrics are exposed via the /inferdeck/metrics endpoint.

#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>
#include <chrono>
#include <mutex>

namespace inferdeck::core {

/// A simple counter metric.
struct CounterMetric {
    uint64_t value = 0;
    std::string description;
};

/// A gauge metric (current value).
struct GaugeMetric {
    double value = 0.0;
    std::string description;
};

/// A histogram metric (min, max, avg, count, sum).
struct HistogramMetric {
    double min_value = 0.0;
    double max_value = 0.0;
    double sum = 0.0;
    uint64_t count = 0;

    double Average() const {
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    }
};

/// MetricsStore provides a thread-safe in-memory store for application metrics.
///
/// Supports counters, gauges, and histograms. Used by the /inferdeck/metrics
/// endpoint to expose monitoring data in JSON format.
class MetricsStore {
public:
    /// Get the singleton MetricsStore instance.
    /// @return Reference to the singleton MetricsStore instance.
    static MetricsStore& Get();

    /// Increment a counter by the given amount.
    /// @param name The metric name.
    /// @param amount Amount to increment by (default: 1).
    void IncrementCounter(const std::string& name, uint64_t amount = 1);

    /// Set a gauge value.
    /// @param name The metric name.
    /// @param value The new gauge value.
    void SetGauge(const std::string& name, double value);

    /// Record a value in a histogram.
    /// @param name The metric name.
    /// @param value The value to record.
    void RecordHistogram(const std::string& name, double value);

    /// Get a counter value.
    /// @param name The metric name.
    /// @return The current counter value, or 0 if not found.
    uint64_t GetCounter(const std::string& name) const;

    /// Get a gauge value.
    /// @param name The metric name.
    /// @return The current gauge value, or 0.0 if not found.
    double GetGauge(const std::string& name) const;

    /// Get histogram statistics.
    /// @param name The metric name.
    /// @return The histogram data, or empty histogram if not found.
    HistogramMetric GetHistogram(const std::string& name) const;

    /// Get all metrics as a JSON string.
    /// @return JSON representation of all metrics.
    std::string ToJson() const;

    /// Reset all metrics to zero.
    void Reset();

private:
    MetricsStore();
    ~MetricsStore();
    MetricsStore(const MetricsStore&) = delete;
    MetricsStore& operator=(const MetricsStore&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, CounterMetric> counters_;
    std::unordered_map<std::string, GaugeMetric> gauges_;
    std::unordered_map<std::string, HistogramMetric> histograms_;
};

} // namespace inferdeck::core
