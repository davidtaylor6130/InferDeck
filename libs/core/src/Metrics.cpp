/// @file Metrics.cpp
/// @brief MetricsStore implementation.

#include "core/Metrics.hpp"
#include <nlohmann/json.hpp>
#include <sstream>

namespace inferdeck::core {

MetricsStore& MetricsStore::Get() {
    static MetricsStore instance;
    return instance;
}

void MetricsStore::IncrementCounter(const std::string& name, uint64_t amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    counters_[name].value += amount;
}

void MetricsStore::SetGauge(const std::string& name, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[name].value = value;
}

void MetricsStore::RecordHistogram(const std::string& name, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& hist = histograms_[name];
    if (hist.count == 0) {
        hist.min_value = value;
        hist.max_value = value;
    } else {
        if (value < hist.min_value) hist.min_value = value;
        if (value > hist.max_value) hist.max_value = value;
    }
    hist.sum += value;
    hist.count++;
}

uint64_t MetricsStore::GetCounter(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = counters_.find(name);
    return it != counters_.end() ? it->second.value : 0;
}

double MetricsStore::GetGauge(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = gauges_.find(name);
    return it != gauges_.end() ? it->second.value : 0.0;
}

HistogramMetric MetricsStore::GetHistogram(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = histograms_.find(name);
    return it != histograms_.end() ? it->second : HistogramMetric{};
}

std::string MetricsStore::ToJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json json = nlohmann::json::object();

    // Counters
    nlohmann::json counters = nlohmann::json::object();
    for (const auto& [name, metric] : counters_) {
        counters[name] = static_cast<nlohmann::json::number_float_t>(metric.value);
    }
    json["counters"] = counters;

    // Gauges
    nlohmann::json gauges = nlohmann::json::object();
    for (const auto& [name, metric] : gauges_) {
        gauges[name] = metric.value;
    }
    json["gauges"] = gauges;

    // Histograms
    nlohmann::json histograms = nlohmann::json::object();
    for (const auto& [name, metric] : histograms_) {
        nlohmann::json hist = nlohmann::json::object();
        hist["min"] = metric.min_value;
        hist["max"] = metric.max_value;
        hist["avg"] = metric.Average();
        hist["count"] = metric.count;
        hist["sum"] = metric.sum;
        histograms[name] = hist;
    }
    json["histograms"] = histograms;

    return json.dump(2);
}

void MetricsStore::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, metric] : counters_) {
        metric.value = 0;
    }
    for (auto& [name, metric] : gauges_) {
        metric.value = 0.0;
    }
    for (auto& [name, metric] : histograms_) {
        metric.min_value = 0.0;
        metric.max_value = 0.0;
        metric.sum = 0.0;
        metric.count = 0;
    }
}

} // namespace inferdeck::core
