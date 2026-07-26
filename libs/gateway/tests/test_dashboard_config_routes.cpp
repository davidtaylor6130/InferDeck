#include <catch2/catch_test_macros.hpp>

#include "foundation/result.hpp"
#include "gateway/dashboard_routes.hpp"
#include "httplib.h"
#include "model/backend_coordinator.hpp"
#include "model/model_registry.hpp"
#include "observability/gpu_telemetry.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using inferdeck::foundation::ErrorCode;
using inferdeck::foundation::Ok;
using inferdeck::gateway::DashboardDeps;
using inferdeck::gateway::GatewayDeps;
using inferdeck::gateway::ProfileBenchmarkManager;
using inferdeck::gateway::ProfileBenchmarkPrompt;
using inferdeck::gateway::ProfileBenchmarkProgress;
using inferdeck::gateway::ProfileBenchmarkTrialMetrics;
using inferdeck::gateway::ProfileBenchmarkTrialRunner;
using inferdeck::gateway::RouteWrapper;
using inferdeck::model::BackendCoordinator;
using inferdeck::model::ModelRegistry;

struct TempConfig {
    fs::path root = fs::temp_directory_path() /
        ("inferdeck-config-routes-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path base = root / "gateway.yml";
    fs::path active = root / "gateway.active.yml";

    TempConfig() {
        fs::create_directories(root);
        write(base, "gateway:\n  host: 127.0.0.1\n  port: 11434\n");
    }

    ~TempConfig() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    static void write(const fs::path& path, const std::string& text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
    }

    static std::string read(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
};

struct ConfigRouteServer {
    ModelRegistry registry;
    BackendCoordinator coordinator{registry};
    inferdeck::observability::GpuTelemetry gpu;
    inferdeck::gateway::SwapTracker swap_tracker;
    std::atomic<bool> maintenance_mode{false};
    std::atomic<int> benchmark_delay_ms{0};
    ProfileBenchmarkTrialRunner benchmark_runner =
        [this](const inferdeck::model::ModelInfo&,
               const inferdeck::optimize::ProfileCandidate& candidate,
               const std::vector<ProfileBenchmarkPrompt>&,
               const std::atomic<bool>& cancel,
               const ProfileBenchmarkProgress& progress) {
            progress("quality", "fake measured quality probe");
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds{benchmark_delay_ms.load()};
            while (std::chrono::steady_clock::now() < deadline) {
                if (cancel.load()) {
                    return inferdeck::foundation::Err<
                        ProfileBenchmarkTrialMetrics>(
                            ErrorCode::Cancelled, "fake trial cancelled");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            ProfileBenchmarkTrialMetrics result;
            result.load_ms = 1250.0;
            result.average_tokens_per_second =
                candidate.cache_type_k == "q4_0" ? 48.0 : 42.0;
            result.parallel_tokens_per_second =
                static_cast<double>(candidate.slots) * 18.0;
            result.average_time_to_first_token_ms = 180.0;
            result.peak_vram_mb = 24000.0;
            result.quality_score =
                candidate.cache_type_k == "q8_0" ? 1.0 : 0.95;
            result.quality_passes = 3;
            result.quality_total = 3;
            result.prompt_tokens = 128;
            result.completion_tokens = 24;
            result.output_samples = {"arithmetic: 714"};
            return inferdeck::foundation::Ok(std::move(result));
        };
    ProfileBenchmarkManager profile_benchmark{
        coordinator,
        &swap_tracker,
        maintenance_mode,
        [this](const inferdeck::model::ModelInfo& info,
               const inferdeck::optimize::ProfileCandidate& candidate,
               const std::vector<ProfileBenchmarkPrompt>& prompts,
               const std::atomic<bool>& cancel,
               const ProfileBenchmarkProgress& progress) {
            return benchmark_runner(
                info, candidate, prompts, cancel, progress);
        }};
    httplib::Server server;
    std::thread thread;
    int port{0};
    std::atomic<int> reloads{0};
    std::function<inferdeck::foundation::Result<void>(const std::string&)> validate =
        [](const std::string&) { return Ok(); };

    explicit ConfigRouteServer(const TempConfig& config) {
        GatewayDeps gateway_deps{
            coordinator, "15", true, {}, {}, nullptr, nullptr, nullptr,
            &swap_tracker, &maintenance_mode};
        DashboardDeps deps{
            gateway_deps,
            gpu,
            {},
            {},
            config.base.string(),
            config.active.string(),
            "running-before-save",
            false,
            {},
            [this](const std::string& text) { return validate(text); },
            [this] {
                reloads.fetch_add(1);
                return Ok();
            },
            nullptr,
            [] { return std::int64_t{1}; },
            &profile_benchmark,
        };
        RouteWrapper direct = [](httplib::Server::Handler handler) { return handler; };
        inferdeck::gateway::register_dashboard_routes(server, deps, direct);
        port = server.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        thread = std::thread([this] { server.listen_after_bind(); });
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    ~ConfigRouteServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }

    httplib::Client client() const {
        return httplib::Client("127.0.0.1", port);
    }
};

}

TEST_CASE("Active configuration save schedules an automatic runtime reload",
          "[gateway][dashboard][config]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    auto client = routes.client();

    const auto current_response = client.Get("/api/config");
    REQUIRE(current_response);
    REQUIRE(current_response->status == 200);
    const auto current = nlohmann::json::parse(current_response->body);

    const std::string updated =
        "gateway:\n  host: 0.0.0.0\n  port: 11434\n";
    const nlohmann::json request{
        {"yaml", updated},
        {"revision", current["activeRevision"]},
    };
    const auto response =
        client.Put("/api/config/active", request.dump(), "application/json");

    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body["ok"] == true);
    CHECK(body["applyScheduled"] == true);
    CHECK(body["restartRequired"] == false);
    CHECK(routes.reloads.load() == 1);
    CHECK(TempConfig::read(config.active) == updated);

    const auto pending_response = client.Get("/api/config");
    REQUIRE(pending_response);
    const auto pending = nlohmann::json::parse(pending_response->body);
    CHECK(pending["restartRequired"] == true);
    CHECK(pending["runningRevision"] == "running-before-save");
    CHECK(pending["activeRevision"] == body["activeRevision"]);
}

TEST_CASE("Invalid active configuration is neither saved nor applied",
          "[gateway][dashboard][config]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    routes.validate = [](const std::string&) {
        return inferdeck::foundation::Err<void>(
            ErrorCode::InvalidArgument, "test validation failure");
    };
    auto client = routes.client();

    const auto current_response = client.Get("/api/config");
    REQUIRE(current_response);
    const auto current = nlohmann::json::parse(current_response->body);
    const nlohmann::json request{
        {"yaml", "invalid: true\n"},
        {"revision", current["activeRevision"]},
    };
    const auto response =
        client.Put("/api/config/active", request.dump(), "application/json");

    REQUIRE(response);
    CHECK(response->status == 400);
    CHECK(routes.reloads.load() == 0);
    CHECK_FALSE(fs::exists(config.active));
}

TEST_CASE("Resetting an active configuration applies the stable baseline",
          "[gateway][dashboard][config]") {
    TempConfig config;
    TempConfig::write(config.active, TempConfig::read(config.base));
    ConfigRouteServer routes(config);
    auto client = routes.client();

    const auto response = client.Delete("/api/config/active");

    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body["removed"] == true);
    CHECK(body["applyScheduled"] == true);
    CHECK(body["restartRequired"] == false);
    CHECK(routes.reloads.load() == 1);
    CHECK_FALSE(fs::exists(config.active));
}

TEST_CASE("Profile analysis returns a quality-first fitting candidate",
          "[gateway][dashboard][optimize]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    inferdeck::model::ModelInfo model;
    model.name = "test-27b";
    model.runtime = "llama_cpp";
    model.gguf_path = "missing-test-artifact.gguf";
    model.context_size = 100000;
    model.n_slots = 4;
    model.min_slots = 1;
    model.vram_required_mb = 24000;
    routes.registry.register_model(model);
    inferdeck::observability::GpuStats gpu;
    gpu.available = true;
    gpu.vram_total_mb = 32768.0;
    gpu.vram_mb = 1024.0;
    gpu.utilization_pct = 1.0;
    routes.gpu.record_external_sample(gpu);
    auto client = routes.client();

    const nlohmann::json request{
        {"model", "test-27b"},
        {"contextPerSlot", 100000},
        {"slots", 4},
        {"minSlots", 1},
        {"nBatch", 2048},
        {"nUbatch", 2048},
        {"cacheTypeK", "q4_0"},
        {"cacheTypeV", "q8_0"},
    };
    const auto response = client.Post(
        "/api/optimize/profile", request.dump(), "application/json");

    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body["mode"] == "profile_estimate");
    CHECK(body["measured"] == false);
    CHECK(body["weights"]["quality"] == 0.60);
    CHECK(body["recommended"]["fits"] == true);
    CHECK(body["recommended"]["contextPerSlot"].get<int>() <= 100000);
    CHECK(body["recommended"]["slots"].get<int>() <= 4);
}

TEST_CASE("Profile analysis refuses to compete with GPU work",
          "[gateway][dashboard][optimize]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    inferdeck::observability::GpuStats gpu;
    gpu.available = true;
    gpu.vram_total_mb = 32768.0;
    gpu.utilization_pct = 50.0;
    routes.gpu.record_external_sample(gpu);
    auto client = routes.client();

    const auto response = client.Post(
        "/api/optimize/profile", R"({"model":"anything"})",
        "application/json");

    REQUIRE(response);
    CHECK(response->status == 409);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body["error"]["code"] == "optimization_busy");
}

TEST_CASE("Measured profile benchmark runs candidates and returns real metrics",
          "[gateway][dashboard][optimize][benchmark]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    inferdeck::model::ModelInfo model;
    model.name = "test-27b";
    model.runtime = "llama_cpp";
    model.context_size = 100000;
    model.n_slots = 4;
    model.min_slots = 1;
    model.vram_required_mb = 24000;
    routes.registry.register_model(model);
    inferdeck::observability::GpuStats gpu;
    gpu.available = true;
    gpu.vram_total_mb = 32768.0;
    gpu.utilization_pct = 1.0;
    routes.gpu.record_external_sample(gpu);
    auto client = routes.client();
    const nlohmann::json request{
        {"model", "test-27b"},
        {"contextPerSlot", 100000},
        {"slots", 4},
        {"minSlots", 1},
        {"nBatch", 2048},
        {"nUbatch", 2048},
        {"cacheTypeK", "q4_0"},
        {"cacheTypeV", "q8_0"},
        {"candidateLimit", 2},
    };

    const auto started = client.Post(
        "/api/optimize/benchmark", request.dump(), "application/json");
    REQUIRE(started);
    CHECK(started->status == 202);
    REQUIRE(routes.profile_benchmark.wait_for_completion(
        std::chrono::seconds{2}));

    const auto response = client.Get("/api/optimize/benchmark");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body["state"] == "completed");
    CHECK(body["measured"] == true);
    CHECK(body["restored"] == true);
    CHECK(body["completedCandidates"] == 2);
    REQUIRE(body["recommended"].is_object());
    CHECK(body["candidates"].size() == 2);
    CHECK(body["candidates"][0]["averageTokensPerSecond"].get<double>() > 0.0);
    CHECK(body["candidates"][0]["qualityTotal"] == 3);
}

TEST_CASE("Measured benchmark blocks model changes and can be cancelled",
          "[gateway][dashboard][optimize][benchmark]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    routes.benchmark_delay_ms.store(500);
    inferdeck::model::ModelInfo model;
    model.name = "test-27b";
    model.runtime = "llama_cpp";
    model.context_size = 100000;
    model.n_slots = 4;
    model.min_slots = 1;
    model.vram_required_mb = 24000;
    routes.registry.register_model(model);
    inferdeck::observability::GpuStats gpu;
    gpu.available = true;
    gpu.vram_total_mb = 32768.0;
    gpu.utilization_pct = 1.0;
    routes.gpu.record_external_sample(gpu);
    auto client = routes.client();
    const nlohmann::json request{
        {"model", "test-27b"},
        {"contextPerSlot", 100000},
        {"slots", 4},
        {"minSlots", 1},
        {"nBatch", 2048},
        {"nUbatch", 2048},
        {"cacheTypeK", "q4_0"},
        {"cacheTypeV", "q8_0"},
        {"candidateLimit", 2},
    };
    const auto started = client.Post(
        "/api/optimize/benchmark", request.dump(), "application/json");
    REQUIRE(started);
    REQUIRE(started->status == 202);

    const auto load = client.Post(
        "/api/models/load", R"({"model":"test-27b"})",
        "application/json");
    REQUIRE(load);
    CHECK(load->status == 503);
    CHECK(nlohmann::json::parse(load->body)["error"]["code"] ==
          "maintenance_mode");

    const auto cancelled = client.Post(
        "/api/optimize/benchmark/cancel", "{}", "application/json");
    REQUIRE(cancelled);
    CHECK(cancelled->status == 202);
    REQUIRE(routes.profile_benchmark.wait_for_completion(
        std::chrono::seconds{2}));
    const auto final = client.Get("/api/optimize/benchmark");
    REQUIRE(final);
    const auto body = nlohmann::json::parse(final->body);
    CHECK(body["state"] == "cancelled");
    CHECK(body["restored"] == true);
    CHECK(routes.maintenance_mode.load() == false);
}
