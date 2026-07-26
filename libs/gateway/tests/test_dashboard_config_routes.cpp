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
    httplib::Server server;
    std::thread thread;
    int port{0};
    std::atomic<int> reloads{0};
    std::function<inferdeck::foundation::Result<void>(const std::string&)> validate =
        [](const std::string&) { return Ok(); };

    explicit ConfigRouteServer(const TempConfig& config) {
        GatewayDeps gateway_deps{coordinator, "15"};
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
