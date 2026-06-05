#include "gateway/metrics_builder.hpp"

namespace inferdeck::gateway {

using nlohmann::json;

namespace {

json gpu_to_json(const observability::GpuStats& s) {
  return {
    {"available", s.available},
    {"provider", s.provider},
    {"reason", s.reason},
    {"timestamp_unix_ms", s.timestamp_unix_ms},
    {"name", s.gpu_name},
    {"utilization_pct", s.utilization_pct},
    {"vram_mb", s.vram_mb},
    {"temperature_c", s.temperature_c},
    {"power_w", s.power_w},
    {"fan_speed_pct", s.fan_speed_pct},
    {"hotspot_temperature_c", s.hotspot_temperature_c}
  };
}

}

json MetricsBuilder::build_live(const observability::Metrics& m,
                                const observability::GpuTelemetry& gpu,
                                std::int64_t uptime_seconds) {
  const auto live = gpu.latest();
  return {
    {"uptime_s", uptime_seconds},
    {"lifetime", {
      {"requests", m.total_requests()},
      {"swaps", m.total_swaps()},
      {"tokens_in", m.lifetime_tokens_in()},
      {"tokens_out", m.lifetime_tokens_out()},
      {"avg_tokens_per_second", m.avg_tokens_per_second()}
    }},
    {"gpu", gpu_to_json(live)}
  };
}

json MetricsBuilder::build_history(const observability::StatsDb& db, int limit) {
  json requests = json::array();
  for (const auto& r : db.recent_requests(limit)) {
    requests.push_back({
      {"timestamp_unix_ms", r.timestamp_unix_ms},
      {"model", r.model},
      {"prompt_tokens", r.prompt_tokens},
      {"completion_tokens", r.completion_tokens},
      {"duration_ms", r.duration_ms},
      {"tokens_per_second", r.tokens_per_second},
      {"status_code", r.status_code},
      {"slot_id", r.slot_id}
    });
  }
  json swaps = json::array();
  for (const auto& s : db.recent_swaps(limit)) {
    swaps.push_back({
      {"timestamp_unix_ms", s.timestamp_unix_ms},
      {"from_model", s.from_model},
      {"to_model", s.to_model},
      {"duration_ms", s.duration_ms},
      {"success", s.success},
      {"error", s.error}
    });
  }
  return {{"requests", std::move(requests)}, {"swaps", std::move(swaps)}};
}

json MetricsBuilder::build_health(const observability::Metrics& m,
                                  const observability::GpuTelemetry& gpu,
                                  const observability::StatsDb& db) {
  const auto live = gpu.latest();
  return {
    {"ok", true},
    {"db_healthy", db.healthy()},
    {"db_path", db.path()},
    {"gpu_available", live.available},
    {"gpu_provider", live.provider},
    {"requests", m.total_requests()}
  };
}

}
