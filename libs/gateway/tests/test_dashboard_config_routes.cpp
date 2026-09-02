#include <catch2/catch_test_macros.hpp>

#include "foundation/result.hpp"
#include "gateway/dashboard_routes.hpp"
#include "gateway/config_repository.hpp"
#include "observability/metrics.hpp"
#include "observability/stats_db.hpp"
#include "gateway/profile_benchmark_scheduler.hpp"
#include "httplib.h"
#include "model/backend_coordinator.hpp"
#include "model/model_registry.hpp"
#include "observability/gpu_telemetry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <yaml-cpp/yaml.h>

namespace {

namespace fs = std::filesystem;
using inferdeck::foundation::ErrorCode;
using inferdeck::foundation::Ok;
using inferdeck::gateway::ApiKeyStore;
using inferdeck::gateway::DashboardDeps;
using inferdeck::gateway::ComputeResource;
using inferdeck::gateway::ConfigRepository;
using inferdeck::gateway::GatewayDeps;
using inferdeck::gateway::ProfileBenchmarkManager;
using inferdeck::gateway::ProfileBenchmarkConcurrencyMetrics;
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
    inferdeck::observability::Metrics metrics;
    inferdeck::observability::StatsDb stats_db{":memory:"};
    std::atomic<ComputeResource> maintenance_resource{ComputeResource::None};
    std::atomic<int> benchmark_delay_ms{0};
    ProfileBenchmarkTrialRunner benchmark_runner =
        [this](const inferdeck::model::ModelInfo& info,
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
            result.prompt_tokens_per_second =
                candidate.cache_type_k == "q4_0" ? 820.0 : 760.0;
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
            for (const int requests : {2, 4}) {
                if (requests > candidate.slots) continue;
                ProfileBenchmarkConcurrencyMetrics concurrency;
                concurrency.requests = requests;
                concurrency.aggregate_tokens_per_second =
                    static_cast<double>(requests) * 18.0;
                concurrency.average_request_tokens_per_second = 18.0;
                if (info.mtp_enabled &&
                    candidate.mtp_max_active_requests >= requests) {
                    concurrency.mtp_requests = requests;
                    concurrency.mtp_drafted_tokens = requests * 100;
                    concurrency.mtp_accepted_tokens = requests * 70;
                    concurrency.aggregate_tokens_per_second +=
                        static_cast<double>(requests) * 5.0;
                    concurrency.average_request_tokens_per_second += 5.0;
                }
                result.concurrency.push_back(std::move(concurrency));
            }
            result.output_samples = {"arithmetic: 714"};
            return inferdeck::foundation::Ok(std::move(result));
        };
    ProfileBenchmarkManager profile_benchmark{
        coordinator,
        &swap_tracker,
        maintenance_resource,
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
    std::shared_ptr<ApiKeyStore> api_keys{
        std::make_shared<ApiKeyStore>(":memory:")};
    std::function<inferdeck::foundation::Result<void>(const std::string&)> validate =
        [](const std::string&) { return Ok(); };
    std::shared_ptr<ConfigRepository> config_repository;

    explicit ConfigRouteServer(const TempConfig& config,
                               std::string pricing_file = {}) {
        config_repository = std::make_shared<ConfigRepository>(
            config.base, config.active,
            [this](const std::string& text) { return validate(text); },
            [this] {
                reloads.fetch_add(1);
                return Ok();
            });
        GatewayDeps gateway_deps{
            coordinator, "15", true, {}, 15000, nullptr, nullptr, nullptr,
            &swap_tracker, &maintenance_resource};
        gateway_deps.metrics = &metrics;
        gateway_deps.stats_db = &stats_db;
        gateway_deps.api_keys = api_keys;
        DashboardDeps deps{
            gateway_deps,
            gpu,
            {},
            std::move(pricing_file),
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
            [] { return std::int64_t{3600}; },
            &profile_benchmark,
            nullptr,
            config_repository,
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

TEST_CASE("Post-training routes report exact native capability boundaries",
          "[gateway][dashboard][post-training]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    auto client = routes.client();

    const auto capabilities = client.Get(
        "/api/inferdeck/v1/post-training/capabilities");
    REQUIRE(capabilities);
    REQUIRE(capabilities->status == 200);
    const auto body = nlohmann::json::parse(capabilities->body);
    CHECK(body["inProcess"] == true);
    CHECK(body["quantization"]["available"] == true);
    CHECK(body["quantization"]["requantization"] == false);
    CHECK(body["quantization"]["cancellable"] == false);
    CHECK(body["quantization"]["computeResource"] == "cpu");
    CHECK(body["quantization"]["blocksNewBackgroundLeases"] == true);
    CHECK(body["quantization"]["types"] ==
          nlohmann::json::array({"Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0"}));
    CHECK(body["fineTuning"]["available"] == false);

    const auto jobs = client.Get(
        "/api/inferdeck/v1/post-training/quantizations");
    REQUIRE(jobs);
    CHECK(jobs->status == 503);
    const auto start = client.Post(
        "/api/inferdeck/v1/post-training/quantizations",
        R"({"sourceModel":"source","outputModel":"output","quantization":"Q4_K_M"})",
        "application/json");
    REQUIRE(start);
    CHECK(start->status == 503);
}

TEST_CASE("API key control routes create, reprioritize, list, and revoke",
          "[gateway][dashboard][api-key]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    auto client = routes.client();

    const auto created = client.Post(
        "/api/inferdeck/v1/api-keys",
        R"({"name":"background worker","priority":-60})",
        "application/json");
    REQUIRE(created);
    REQUIRE(created->status == 201);
    CHECK(created->get_header_value("Cache-Control") == "no-store");
    const auto created_body = nlohmann::json::parse(created->body);
    const std::string id = created_body["id"].get<std::string>();
    const std::string key = created_body["key"].get<std::string>();
    REQUIRE(routes.api_keys->authenticate_bearer("Bearer " + key));

    const auto updated = client.Patch(
        "/api/inferdeck/v1/api-keys/" + id,
        R"({"priority":45})", "application/json");
    REQUIRE(updated);
    REQUIRE(updated->status == 200);
    CHECK(nlohmann::json::parse(updated->body)["priority"] == 45);
    REQUIRE(routes.api_keys->authenticate_bearer("Bearer " + key));
    CHECK(routes.api_keys->authenticate_bearer("Bearer " + key)->priority == 45);

    const auto listed = client.Get("/api/inferdeck/v1/api-keys");
    REQUIRE(listed);
    REQUIRE(listed->status == 200);
    CHECK(listed->body.find(key) == std::string::npos);
    const auto listed_body = nlohmann::json::parse(listed->body);
    REQUIRE(listed_body["apiKeys"].size() == 1);
    CHECK_FALSE(listed_body["apiKeys"][0].contains("key"));

    const auto revoked = client.Delete("/api/inferdeck/v1/api-keys/" + id);
    REQUIRE(revoked);
    CHECK(revoked->status == 204);
    CHECK_FALSE(routes.api_keys->authenticate_bearer("Bearer " + key));
}

TEST_CASE("API settings safely persist public data-plane access",
          "[gateway][dashboard][api-settings]") {
    TempConfig config;
    TempConfig::write(config.base,
        "auth:\n"
        "  required: true\n"
        "  token: preserved-secret\n"
        "gateway:\n"
        "  auto_swap: true\n");
    ConfigRouteServer routes(config);
    auto client = routes.client();

    const auto initial = client.Get("/api/inferdeck/v1/api-settings");
    REQUIRE(initial);
    REQUIRE(initial->status == 200);
    const auto initial_body = nlohmann::json::parse(initial->body);
    CHECK(initial_body["allowPublicTraffic"] == false);
    CHECK(initial_body["runningAllowPublicTraffic"] == false);
    CHECK(initial_body["publicPriority"] == -999999);
    CHECK(initial->body.find("preserved-secret") == std::string::npos);

    const std::string revision =
        initial_body["activeRevision"].get<std::string>();
    const auto updated = client.Put(
        "/api/inferdeck/v1/api-settings",
        nlohmann::json{{"allowPublicTraffic", true},
                       {"revision", revision}}.dump(),
        "application/json");
    REQUIRE(updated);
    REQUIRE(updated->status == 200);
    const auto updated_body = nlohmann::json::parse(updated->body);
    CHECK(updated_body["allowPublicTraffic"] == true);
    CHECK(updated_body["publicPriority"] == -999999);
    CHECK(updated_body["applyScheduled"] == true);
    CHECK(routes.reloads.load() == 1);

    const auto active = YAML::Load(TempConfig::read(config.active));
    REQUIRE(active["auth"]);
    CHECK(active["auth"]["required"].as<bool>() == false);
    CHECK(active["auth"]["token"].as<std::string>() == "preserved-secret");
    CHECK(active["gateway"]["auto_swap"].as<bool>() == true);

    const auto stale = client.Put(
        "/api/inferdeck/v1/api-settings",
        nlohmann::json{{"allowPublicTraffic", false},
                       {"revision", revision}}.dump(),
        "application/json");
    REQUIRE(stale);
    CHECK(stale->status == 409);

    const auto invalid = client.Put(
        "/api/inferdeck/v1/api-settings",
        R"({"allowPublicTraffic":true,"revision":7})",
        "application/json");
    REQUIRE(invalid);
    CHECK(invalid->status == 400);
}

TEST_CASE("Background lease routes require managed keys and report conflicts",
          "[gateway][dashboard][background-lease]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    auto client = routes.client();
    const auto first_key = routes.api_keys->create("first worker", -60);
    const auto second_key = routes.api_keys->create("second worker", -40);
    REQUIRE(first_key);
    REQUIRE(second_key);
    const httplib::Headers first_headers{
        {"Authorization", "Bearer " + first_key->key}};
    const httplib::Headers second_headers{
        {"Authorization", "Bearer " + second_key->key}};
    constexpr const char* availability_path =
        "/api/inferdeck/v1/background/availability";
    constexpr const char* lease_path =
        "/api/inferdeck/v1/background/lease";

    const auto unauthenticated = client.Get(availability_path);
    REQUIRE(unauthenticated);
    CHECK(unauthenticated->status == 401);

    const auto too_short = client.Post(
        lease_path, first_headers, R"({"durationSeconds":10})",
        "application/json");
    REQUIRE(too_short);
    CHECK(too_short->status == 400);
    const auto too_large = client.Post(
        lease_path, first_headers,
        R"({"durationSeconds":18446744073709551615})",
        "application/json");
    REQUIRE(too_large);
    CHECK(too_large->status == 400);
    const auto unknown_field = client.Post(
        lease_path, first_headers, R"({"durationSeconds":120,"owner":"x"})",
        "application/json");
    REQUIRE(unknown_field);
    CHECK(unknown_field->status == 400);

    const auto available = client.Get(availability_path, first_headers);
    REQUIRE(available);
    REQUIRE(available->status == 200);
    const auto available_body = nlohmann::json::parse(available->body);
    CHECK(available_body["available"] == true);
    CHECK(available_body["reason"] == "idle");

    const auto acquired = client.Post(
        lease_path, first_headers, R"({"durationSeconds":120})",
        "application/json");
    REQUIRE(acquired);
    REQUIRE(acquired->status == 201);
    CHECK(acquired->get_header_value("Cache-Control") == "no-store");
    const auto acquired_body = nlohmann::json::parse(acquired->body);
    CHECK(acquired_body["status"] == "acquired");
    const std::string lease_id =
        acquired_body["lease"]["id"].get<std::string>();
    const std::int64_t original_expiry =
        acquired_body["lease"]["expiresAtUnixMs"].get<std::int64_t>();

    const auto conflict = client.Post(
        lease_path, second_headers, R"({"durationSeconds":120})",
        "application/json");
    REQUIRE(conflict);
    REQUIRE(conflict->status == 409);
    CHECK_FALSE(conflict->get_header_value("Retry-After").empty());
    const auto conflict_body = nlohmann::json::parse(conflict->body);
    CHECK(conflict_body["reason"] == "lease_active");
    CHECK(conflict_body.contains("suggestedReportBackAtUnixMs"));
    CHECK(conflict_body.contains("expiresAtUnixMs"));
    CHECK(conflict->body.find(first_key->record.id) == std::string::npos);
    CHECK(conflict->body.find(first_key->record.name) == std::string::npos);
    CHECK(conflict->body.find(lease_id) == std::string::npos);

    const auto repeated = client.Post(
        lease_path, first_headers, R"({"durationSeconds":240})",
        "application/json");
    REQUIRE(repeated);
    REQUIRE(repeated->status == 200);
    const auto repeated_body = nlohmann::json::parse(repeated->body);
    CHECK(repeated_body["status"] == "existing");
    CHECK(repeated_body["lease"]["id"] == lease_id);
    CHECK(repeated_body["lease"]["expiresAtUnixMs"] == original_expiry);

    const auto renewed = client.Patch(
        std::string(lease_path) + "/" + lease_id, first_headers,
        R"({"durationSeconds":180})", "application/json");
    REQUIRE(renewed);
    REQUIRE(renewed->status == 200);
    const auto renewed_body = nlohmann::json::parse(renewed->body);
    CHECK(renewed_body["lease"]["id"] == lease_id);
    CHECK(renewed_body["lease"]["expiresAtUnixMs"].get<std::int64_t>() >
          original_expiry);

    const auto wrong_release = client.Delete(
        std::string(lease_path) + "/" + lease_id, second_headers);
    REQUIRE(wrong_release);
    CHECK(wrong_release->status == 204);
    const auto still_owned = client.Get(availability_path, first_headers);
    REQUIRE(still_owned);
    REQUIRE(still_owned->status == 200);
    const auto still_owned_body = nlohmann::json::parse(still_owned->body);
    CHECK(still_owned_body["reason"] == "lease_owned");
    CHECK(still_owned_body["lease"]["id"] == lease_id);

    const auto released = client.Delete(
        std::string(lease_path) + "/" + lease_id, first_headers);
    REQUIRE(released);
    CHECK(released->status == 204);
    const auto replacement = client.Post(
        lease_path, second_headers, R"({"durationSeconds":120})",
        "application/json");
    REQUIRE(replacement);
    CHECK(replacement->status == 201);
}

TEST_CASE("Background lease acquisition waits for the configured quiet period",
          "[gateway][dashboard][background-lease]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    auto client = routes.client();
    const auto key = routes.api_keys->create("quiet worker", -60);
    REQUIRE(key);
    const httplib::Headers headers{
        {"Authorization", "Bearer " + key->key}};
    routes.maintenance_resource.store(ComputeResource::Cpu);
    const auto maintenance = client.Get(
        "/api/inferdeck/v1/background/availability", headers);
    REQUIRE(maintenance);
    REQUIRE(maintenance->status == 200);
    const auto maintenance_body = nlohmann::json::parse(maintenance->body);
    CHECK(maintenance_body["available"] == false);
    CHECK(maintenance_body["reason"] == "maintenance");
    CHECK(maintenance_body.contains("suggestedReportBackAtUnixMs"));
    const auto maintenance_lease = client.Post(
        "/api/inferdeck/v1/background/lease", headers,
        R"({"durationSeconds":120})", "application/json");
    REQUIRE(maintenance_lease);
    REQUIRE(maintenance_lease->status == 409);
    CHECK(nlohmann::json::parse(maintenance_lease->body)["reason"] ==
          "maintenance");
    routes.maintenance_resource.store(ComputeResource::None);

    const std::int64_t now = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    routes.stats_db.record_request(
        {now, "interactive-model", 10, 5, 100.0, 50.0, 200, 0});

    const auto availability = client.Get(
        "/api/inferdeck/v1/background/availability", headers);
    REQUIRE(availability);
    REQUIRE(availability->status == 200);
    CHECK_FALSE(availability->get_header_value("Retry-After").empty());
    const auto availability_body =
        nlohmann::json::parse(availability->body);
    CHECK(availability_body["available"] == false);
    CHECK(availability_body["reason"] == "quiet_period");
    CHECK(availability_body["requiredIdleSeconds"] == 900);
    CHECK(availability_body["suggestedReportBackAtUnixMs"]
              .get<std::int64_t>() > now);

    const auto blocked = client.Post(
        "/api/inferdeck/v1/background/lease", headers,
        R"({"durationSeconds":120})", "application/json");
    REQUIRE(blocked);
    REQUIRE(blocked->status == 409);
    CHECK_FALSE(blocked->get_header_value("Retry-After").empty());
    const auto blocked_body = nlohmann::json::parse(blocked->body);
    CHECK(blocked_body["error"]["code"] == "background_not_idle");
    CHECK(blocked_body["reason"] == "quiet_period");
    CHECK(blocked_body.contains("suggestedReportBackAtUnixMs"));
}

TEST_CASE("Active configuration save schedules an automatic runtime reload",
          "[gateway][dashboard][config]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    auto client = routes.client();

    const auto current_response = client.Get("/api/inferdeck/v1/config");
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
        client.Put("/api/inferdeck/v1/config/active", request.dump(), "application/json");

    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body["ok"] == true);
    CHECK(body["applyScheduled"] == true);
    CHECK(body["restartRequired"] == false);
    CHECK(routes.reloads.load() == 1);
    CHECK(TempConfig::read(config.active) == updated);

    const auto pending_response = client.Get("/api/inferdeck/v1/config");
    REQUIRE(pending_response);
    const auto pending = nlohmann::json::parse(pending_response->body);
    CHECK(pending["restartRequired"] == true);
    CHECK(pending["runningRevision"] == "running-before-save");
    CHECK(pending["activeRevision"] == body["activeRevision"]);
}

TEST_CASE("Configuration API masks and restores every credential",
          "[gateway][dashboard][config][security]") {
    TempConfig config;
    const std::string original =
        "auth:\n"
        "  required: true\n"
        "  token: data-secret\n"
        "control:\n"
        "  allow_remote: true\n"
        "  token: control-secret\n"
        "  origins: [https://admin.example]\n"
        "model_store:\n"
        "  hf_token: hf-secret\n";
    TempConfig::write(config.base, original);
    ConfigRouteServer routes(config);
    auto client = routes.client();

    const auto current_response = client.Get("/api/inferdeck/v1/config");
    REQUIRE(current_response);
    REQUIRE(current_response->status == 200);
    const auto current = nlohmann::json::parse(current_response->body);
    const auto masked = current["yaml"].get<std::string>();
    CHECK(masked.find("data-secret") == std::string::npos);
    CHECK(masked.find("control-secret") == std::string::npos);
    CHECK(masked.find("hf-secret") == std::string::npos);
    std::size_t sentinel_count = 0;
    std::size_t position = 0;
    while ((position = masked.find("__INFERDECK_SECRET__", position)) !=
           std::string::npos) {
        ++sentinel_count;
        position += std::string_view{"__INFERDECK_SECRET__"}.size();
    }
    CHECK(sentinel_count == 3);

    const nlohmann::json request{
        {"yaml", masked},
        {"revision", current["revision"]},
    };
    const auto response = client.Put(
        "/api/inferdeck/v1/config", request.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    CHECK(TempConfig::read(config.base) == original);
}

TEST_CASE("Configuration API redacts noncanonical YAML secret forms",
          "[gateway][dashboard][config][security]") {
    TempConfig config;
    const std::string original =
        "auth: {required: true, token: flow-data-secret}\n"
        "\"control\":\n"
        "  allow_remote: false\n"
        "  \"token\": >-\n"
        "    block-control-secret\n"
        "model_store:\n"
        "  hf_token: 'quoted-hf-secret'\n";
    TempConfig::write(config.base, original);
    ConfigRouteServer routes(config);
    auto client = routes.client();

    const auto current_response = client.Get("/api/inferdeck/v1/config");
    REQUIRE(current_response);
    REQUIRE(current_response->status == 200);
    const auto current = nlohmann::json::parse(current_response->body);
    const auto masked = current["yaml"].get<std::string>();
    CHECK(masked.find("flow-data-secret") == std::string::npos);
    CHECK(masked.find("block-control-secret") == std::string::npos);
    CHECK(masked.find("quoted-hf-secret") == std::string::npos);
    const auto masked_yaml = YAML::Load(masked);
    CHECK(masked_yaml["auth"]["token"].as<std::string>() == "__INFERDECK_SECRET__");
    CHECK(masked_yaml["control"]["token"].as<std::string>() == "__INFERDECK_SECRET__");
    CHECK(masked_yaml["model_store"]["hf_token"].as<std::string>() ==
          "__INFERDECK_SECRET__");

    const nlohmann::json request{
        {"yaml", masked},
        {"revision", current["revision"]},
    };
    const auto response = client.Put(
        "/api/inferdeck/v1/config", request.dump(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto restored = YAML::Load(TempConfig::read(config.base));
    CHECK(restored["auth"]["token"].as<std::string>() == "flow-data-secret");
    CHECK(restored["control"]["token"].as<std::string>() == "block-control-secret");
    CHECK(restored["model_store"]["hf_token"].as<std::string>() ==
          "quoted-hf-secret");
}

TEST_CASE("Configuration API masks duplicate secret keys without disclosure",
          "[gateway][dashboard][config][security]") {
    TempConfig config;
    TempConfig::write(config.base,
        "control:\n  token: first-control-secret\n  token: second-control-secret\n");
    ConfigRouteServer routes(config);
    auto client = routes.client();

    const auto response = client.Get("/api/inferdeck/v1/config");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto masked = nlohmann::json::parse(response->body)["yaml"].get<std::string>();
    CHECK(masked.find("first-control-secret") == std::string::npos);
    CHECK(masked.find("second-control-secret") == std::string::npos);
}

TEST_CASE("Model alias API persists CRUD changes and compatibility contract",
          "[gateway][dashboard][aliases]") {
    TempConfig config;
    TempConfig::write(config.base,
        "# keep this operator note\ngateway:\n  host: 127.0.0.1\n  port: 11434\n");
    ConfigRouteServer routes(config);
    inferdeck::model::ModelInfo target;
    target.name = "concrete-model";
    target.gguf_path = "C:/fake/model.gguf";
    target.context_size = 32768;
    target.capabilities = {"chat_completions", "responses"};
    routes.registry.register_model(target);
    auto client = routes.client();

    const auto initial_aliases = client.Get("/api/inferdeck/v1/model-aliases");
    REQUIRE(initial_aliases);
    const auto initial_alias_document = nlohmann::json::parse(initial_aliases->body);
    const nlohmann::json create{
        {"target", "concrete-model"},
        {"requiredContextSize", 16384},
        {"requiredCapabilities", nlohmann::json::array({"chat_completions"})},
        {"revision", initial_alias_document["revision"]},
    };
    const auto created = client.Put(
        "/api/inferdeck/v1/model-aliases/stable-chat", create.dump(), "application/json");
    REQUIRE(created);
    REQUIRE(created->status == 201);
    const auto created_body = nlohmann::json::parse(created->body);
    CHECK(created_body["name"] == "stable-chat");
    CHECK(created_body["target"] == "concrete-model");
    CHECK(created_body["requiredContextSize"] == 16384);

    const auto listed = client.Get("/api/inferdeck/v1/model-aliases");
    REQUIRE(listed);
    REQUIRE(listed->status == 200);
    CHECK(nlohmann::json::parse(listed->body)["aliases"].size() == 1);
    const auto persisted = YAML::Load(TempConfig::read(config.active));
    CHECK(TempConfig::read(config.active).find("# keep this operator note") !=
          std::string::npos);
    REQUIRE(persisted["model_aliases"]);
    CHECK(persisted["model_aliases"][0]["name"].as<std::string>() == "stable-chat");

    httplib::Headers delete_headers{
        {"If-Match", nlohmann::json::parse(listed->body)["revision"].get<std::string>()}};
    const auto removed = client.Delete(
        "/api/inferdeck/v1/model-aliases/stable-chat", delete_headers);
    REQUIRE(removed);
    REQUIRE(removed->status == 200);
    CHECK(routes.registry.aliases().empty());
    const auto after_delete = YAML::Load(TempConfig::read(config.active));
    CHECK(after_delete["model_aliases"].size() == 0);
}

TEST_CASE("Pricing API exposes cached input rates for models and aliases",
          "[gateway][dashboard][pricing]") {
    TempConfig config;
    const auto pricing_path = config.root / "pricing.json";
    TempConfig::write(pricing_path, R"([
      {
        "model_name": "priced-model",
        "prompt_price_per_million": 0.45,
        "cached_prompt_price_per_million": 0.05,
        "completion_price_per_million": 3.2
      },
      {
        "model_name": "cached-only",
        "prompt_price_per_million": 1.0,
        "cached_prompt_price_per_million": 0.1,
        "completion_price_per_million": 2.0
      },
      {
        "model_name": "prompt-only",
        "prompt_price_per_million": 0.5,
        "cached_prompt_price_per_million": 0.05,
        "completion_price_per_million": 1.0
      },
      {
        "model_name": "completion-only",
        "prompt_price_per_million": 0.4,
        "cached_prompt_price_per_million": 0.04,
        "completion_price_per_million": 0.8
      }
    ])");
    ConfigRouteServer routes(config, pricing_path.string());
    inferdeck::model::ModelInfo model;
    model.name = "priced-model";
    model.prompt_price_per_million = 0.45;
    model.cached_prompt_price_per_million = 0.04;
    model.completion_price_per_million = 3.2;
    routes.registry.register_model(model);
    inferdeck::model::ModelInfo cached_only;
    cached_only.name = "cached-only";
    cached_only.cached_prompt_price_per_million = 0.02;
    routes.registry.register_model(cached_only);
    inferdeck::model::ModelInfo prompt_only;
    prompt_only.name = "prompt-only";
    prompt_only.prompt_price_per_million = 0.6;
    routes.registry.register_model(prompt_only);
    inferdeck::model::ModelInfo completion_only;
    completion_only.name = "completion-only";
    completion_only.completion_price_per_million = 0.9;
    routes.registry.register_model(completion_only);
    inferdeck::model::ModelInfo new_cached_only;
    new_cached_only.name = "new-cached-only";
    new_cached_only.cached_prompt_price_per_million = 0.03;
    routes.registry.register_model(new_cached_only);
    inferdeck::model::ModelInfo new_prompt_only;
    new_prompt_only.name = "new-prompt-only";
    new_prompt_only.prompt_price_per_million = 0.7;
    routes.registry.register_model(new_prompt_only);
    inferdeck::model::ModelInfo new_completion_only;
    new_completion_only.name = "new-completion-only";
    new_completion_only.completion_price_per_million = 1.1;
    routes.registry.register_model(new_completion_only);
    inferdeck::model::ModelAlias alias;
    alias.name = "stable-priced-model";
    alias.target = model.name;
    REQUIRE(routes.registry.set_alias(alias));

    auto client = routes.client();
    const auto response = client.Get("/api/inferdeck/v1/pricing");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    const auto find_price = [&body](const std::string& name) {
        return std::find_if(body.begin(), body.end(), [&name](const auto& entry) {
            return entry.value("model_name", "") == name;
        });
    };
    const auto priced = find_price("priced-model");
    const auto aliased = find_price("stable-priced-model");
    REQUIRE(priced != body.end());
    REQUIRE(aliased != body.end());
    CHECK((*priced)["prompt_price_per_million"] == 0.45);
    CHECK((*priced)["cached_prompt_price_per_million"] == 0.04);
    CHECK((*priced)["completion_price_per_million"] == 3.2);
    CHECK((*aliased)["cached_prompt_price_per_million"] == 0.04);
    CHECK((*aliased)["source"] == "model_alias");
    const auto cached = find_price("cached-only");
    const auto prompt = find_price("prompt-only");
    const auto completion = find_price("completion-only");
    REQUIRE(cached != body.end());
    REQUIRE(prompt != body.end());
    REQUIRE(completion != body.end());
    CHECK((*cached)["prompt_price_per_million"] == 1.0);
    CHECK((*cached)["cached_prompt_price_per_million"] == 0.02);
    CHECK((*cached)["completion_price_per_million"] == 2.0);
    CHECK((*prompt)["prompt_price_per_million"] == 0.6);
    CHECK((*prompt)["cached_prompt_price_per_million"] == 0.05);
    CHECK((*prompt)["completion_price_per_million"] == 1.0);
    CHECK((*completion)["prompt_price_per_million"] == 0.4);
    CHECK((*completion)["cached_prompt_price_per_million"] == 0.04);
    CHECK((*completion)["completion_price_per_million"] == 0.9);
    const auto new_cached = find_price("new-cached-only");
    const auto new_prompt = find_price("new-prompt-only");
    const auto new_completion = find_price("new-completion-only");
    REQUIRE(new_cached != body.end());
    REQUIRE(new_prompt != body.end());
    REQUIRE(new_completion != body.end());
    CHECK((*new_cached)["prompt_price_per_million"] == 0.0);
    CHECK((*new_cached)["cached_prompt_price_per_million"] == 0.03);
    CHECK((*new_cached)["completion_price_per_million"] == 0.0);
    CHECK((*new_prompt)["prompt_price_per_million"] == 0.7);
    CHECK((*new_prompt)["cached_prompt_price_per_million"] == 0.7);
    CHECK((*new_prompt)["completion_price_per_million"] == 0.0);
    CHECK((*new_completion)["prompt_price_per_million"] == 0.0);
    CHECK((*new_completion)["cached_prompt_price_per_million"] == 0.0);
    CHECK((*new_completion)["completion_price_per_million"] == 1.1);
}

TEST_CASE("Usage API exposes daily usage for the complete retained history",
          "[gateway][dashboard][usage]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    routes.stats_db.record_request({
        1704067200000LL, "old-model", 100, 50, 0.0, 0.0, 200, -1});
    routes.stats_db.record_request({
        1787011200000LL, "new-model", 10, 5, 0.0, 0.0, 200, -1});
    inferdeck::observability::RequestRow image;
    image.timestamp_unix_ms = 1787011201000LL;
    image.model = "stable-diffusion-v1-5";
    image.modality = "image_generation";
    image.status_code = 200;
    image.generation_duration_ms = 4'000.0;
    image.output_image_count = 2;
    routes.stats_db.record_request(image);
    inferdeck::observability::RequestRow music;
    music.timestamp_unix_ms = 1787011202000LL;
    music.model = "ace-step-v1.5";
    music.modality = "audio_generation";
    music.status_code = 200;
    music.generation_duration_ms = 6'000.0;
    music.output_audio_seconds = 10.0;
    routes.stats_db.record_request(music);
    auto client = routes.client();
    const auto status_response = client.Get("/api/inferdeck/v1/status");
    REQUIRE(status_response);
    REQUIRE(status_response->status == 200);
    const auto status = nlohmann::json::parse(status_response->body);
    CHECK(status["dailyTokenUsageAllTime"] == false);
    const auto sum_tokens = [](const auto& rows) {
        std::int64_t total = 0;
        for (const auto& row : rows) total += row.value("totalTokens", 0LL);
        return total;
    };
    CHECK(sum_tokens(status["tokenUsage"]) == 165);
    CHECK(sum_tokens(status["monthlyTokenUsage"]) == 165);
    const auto find_model = [](const auto& rows, const std::string& model) {
        return std::find_if(rows.begin(), rows.end(), [&model](const auto& row) {
            return row.value("model", "") == model;
        });
    };
    const auto image_usage =
        find_model(status["tokenUsage"], "stable-diffusion-v1-5");
    const auto music_usage = find_model(status["tokenUsage"], "ace-step-v1.5");
    REQUIRE(image_usage != status["tokenUsage"].end());
    REQUIRE(music_usage != status["tokenUsage"].end());
    CHECK((*image_usage)["outputImageCount"] == 2);
    CHECK((*image_usage)["generationDurationMs"] == 4'000.0);
    CHECK((*music_usage)["outputAudioSeconds"] == 10.0);
    CHECK((*music_usage)["generationDurationMs"] == 6'000.0);
    const auto response = client.Get("/api/inferdeck/v1/usage/daily");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body["dailyTokenUsageAllTime"] == true);
    const auto image_daily =
        find_model(body["dailyTokenUsage"], "stable-diffusion-v1-5");
    const auto music_daily =
        find_model(body["dailyTokenUsage"], "ace-step-v1.5");
    REQUIRE(image_daily != body["dailyTokenUsage"].end());
    REQUIRE(music_daily != body["dailyTokenUsage"].end());
    CHECK((*image_daily)["outputImageCount"] == 2);
    CHECK((*music_daily)["outputAudioSeconds"] == 10.0);
    CHECK(std::any_of(
        body["dailyTokenUsage"].begin(), body["dailyTokenUsage"].end(),
        [](const auto& row) {
            return row.value("model", "") == "old-model" &&
                   row.value("bucket", "") == "2024-01-01";
        }));
}

TEST_CASE("Control model inventory retains InferDeck runtime fields",
          "[gateway][dashboard][models]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    inferdeck::model::ModelInfo info;
    info.name = "dashboard-model";
    info.family = "dashboard-family";
    info.runtime = "llama_cpp";
    info.modality = "text";
    info.capabilities = {"chat_completions"};
    info.context_size = 65536;
    info.vram_required_mb = 8192;
    info.n_slots = 2;
    info.reasoning.supported = true;
    info.reasoning.efforts = {"low", "high"};
    info.reasoning.default_effort = "high";
    info.reasoning.none_disables = true;
    routes.registry.register_model(info);

    auto client = routes.client();
    const auto response = client.Get("/api/inferdeck/v1/models");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body["models"].size() == 1);
    CHECK(body["models"][0]["id"] == "dashboard-model");
    CHECK(body["models"][0]["runtime"] == "llama_cpp");
    CHECK(body["models"][0]["context_size"] == 65536);
    CHECK(body["models"][0]["vram_required_mb"] == 8192);
    CHECK(body["models"][0]["reasoning"]["supported"] == true);
    CHECK(body["models"][0]["reasoning"]["efforts"] ==
          nlohmann::json::array({"low", "high"}));
    CHECK(body["models"][0]["reasoning"]["default"] == "high");
    CHECK(body["models"][0]["reasoning"]["none_disables"] == true);
    CHECK(body["models"][0]["loaded"] == false);
}

TEST_CASE("Configured external models can be unregistered without rewriting unrelated config",
          "[gateway][dashboard][models]") {
    TempConfig config;
    TempConfig::write(config.base,
        "# keep this header\n"
        "default_model: keep-model\n"
        "model_registry:\n"
        "  - name: external-model\n"
        "    runtime: llama_cpp\n"
        "    gguf_path: C:/models/external.gguf\n"
        "  # keep next model note\n"
        "  - name: keep-model\n"
        "    runtime: llama_cpp\n"
        "    gguf_path: C:/models/keep.gguf\n"
        "observability:\n"
        "  # keep this setting\n"
        "  telemetry_poll_ms: 1000\n");
    ConfigRouteServer routes(config);
    inferdeck::model::ModelInfo external;
    external.name = "external-model";
    external.runtime = "llama_cpp";
    routes.registry.register_model(external);

    auto client = routes.client();
    const auto current_response = client.Get("/api/inferdeck/v1/config");
    REQUIRE(current_response);
    const auto current = nlohmann::json::parse(current_response->body);
    const nlohmann::json request{
        {"model", "external-model"},
        {"revision", current["activeRevision"]},
    };
    const auto removed = client.Post(
        "/api/inferdeck/v1/model-store/unregister", request.dump(),
        "application/json");
    REQUIRE(removed);
    CHECK(removed->status == 200);
    const auto body = nlohmann::json::parse(removed->body);
    CHECK(body["filesDeleted"] == false);
    CHECK_FALSE(routes.registry.has("external-model"));
    const auto active = TempConfig::read(config.active);
    CHECK(active.find("external-model") == std::string::npos);
    CHECK(active.find("keep-model") != std::string::npos);
    CHECK(active.find("# keep this header") != std::string::npos);
    CHECK(active.find("# keep next model note") != std::string::npos);
    CHECK(active.find("# keep this setting") != std::string::npos);
}

TEST_CASE("Scheduled optimization starts once while the gateway is idle",
          "[gateway][dashboard][optimization][schedule]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    inferdeck::model::ModelInfo model;
    model.name = "scheduled-model";
    model.gguf_path = "C:/fake/scheduled-model.gguf";
    model.context_size = 32768;
    model.n_slots = 1;
    model.min_slots = 1;
    model.vram_required_mb = 8192;
    model.optimization.schedule_enabled = true;
    model.optimization.schedule_window_start = "00:00";
    model.optimization.schedule_window_end = "23:59";
    routes.registry.register_model(model);
    inferdeck::observability::GpuStats sample;
    sample.available = true;
    sample.vram_total_mb = 32768;
    sample.utilization_pct = 0;
    routes.gpu.record_external_sample(sample);

    inferdeck::gateway::ProfileBenchmarkScheduler scheduler(
        routes.profile_benchmark, routes.coordinator, routes.gpu);
    scheduler.evaluate();
    REQUIRE(routes.profile_benchmark.wait_for_completion(
        std::chrono::seconds{2}));
    scheduler.evaluate();
    const auto statuses = scheduler.statuses();
    const auto status = std::find_if(statuses.begin(), statuses.end(), [](const auto& entry) {
        return entry.model == "scheduled-model";
    });
    REQUIRE(status != statuses.end());
    CHECK(status->last_started_unix_ms > 0);
    CHECK(status->last_finished_unix_ms > 0);
    CHECK(status->last_outcome == "completed");
    const auto first_started = status->last_started_unix_ms;
    scheduler.evaluate();
    const auto repeated = scheduler.statuses();
    const auto repeated_status = std::find_if(
        repeated.begin(), repeated.end(), [](const auto& entry) {
            return entry.model == "scheduled-model";
        });
    REQUIRE(repeated_status != repeated.end());
    CHECK(repeated_status->last_started_unix_ms == first_started);
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

    const auto current_response = client.Get("/api/inferdeck/v1/config");
    REQUIRE(current_response);
    const auto current = nlohmann::json::parse(current_response->body);
    const nlohmann::json request{
        {"yaml", "invalid: true\n"},
        {"revision", current["activeRevision"]},
    };
    const auto response =
        client.Put("/api/inferdeck/v1/config/active", request.dump(), "application/json");

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

    const auto current_response = client.Get("/api/inferdeck/v1/config");
    REQUIRE(current_response);
    const auto current = nlohmann::json::parse(current_response->body);
    httplib::Headers headers{{"If-Match", current["activeRevision"].get<std::string>()}};
    const auto response = client.Delete("/api/inferdeck/v1/config/active", headers);

    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body["removed"] == true);
    CHECK(body["applyScheduled"] == true);
    CHECK(body["restartRequired"] == false);
    CHECK(routes.reloads.load() == 1);
    CHECK_FALSE(fs::exists(config.active));
}

TEST_CASE("Profile benchmark teardown preserves another maintenance owner",
          "[gateway][dashboard][optimize][maintenance]") {
    inferdeck::model::ModelRegistry registry;
    BackendCoordinator coordinator{registry};
    inferdeck::gateway::SwapTracker swap_tracker;
    std::atomic<ComputeResource> maintenance_resource{ComputeResource::Cpu};
    {
        ProfileBenchmarkManager benchmark{
            coordinator, &swap_tracker, maintenance_resource,
            inferdeck::gateway::ProfileBenchmarkTrialRunner{}};
    }
    CHECK(maintenance_resource.load() == ComputeResource::Cpu);
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
        "/api/inferdeck/v1/optimize/profile", request.dump(), "application/json");

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
        "/api/inferdeck/v1/optimize/profile", R"({"model":"anything"})",
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
        "/api/inferdeck/v1/optimize/benchmark", request.dump(), "application/json");
    REQUIRE(started);
    CHECK(started->status == 202);
    REQUIRE(routes.profile_benchmark.wait_for_completion(
        std::chrono::seconds{2}));

    const auto response = client.Get("/api/inferdeck/v1/optimize/benchmark");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body["state"] == "completed");
    CHECK(body["measured"] == true);
    CHECK(body["restored"] == true);
    CHECK(body["completedCandidates"] == 3);
    REQUIRE(body["baseline"].is_object());
    CHECK(body["baseline"]["completed"] == true);
    CHECK(body["baseline"]["qualityTotal"] == 3);
    CHECK(body["baseline"]["performanceIndex"] == 100.0);
    REQUIRE(body["recommended"].is_object());
    CHECK(body["candidates"].size() == 2);
    CHECK(body["candidates"][0]["averageTokensPerSecond"].get<double>() > 0.0);
    CHECK(body["candidates"][0]["promptTokensPerSecond"].get<double>() > 0.0);
    CHECK(body["candidates"][0]["performanceIndex"].get<double>() > 0.0);
    CHECK(body["candidates"][0]["qualityTotal"] == 3);
    CHECK(body["candidates"][0]["reserveVramMb"] == 8768.0);
    CHECK(body["candidates"][0]["speedScore"].get<double>() <= 1.0);
    CHECK(body["candidates"][1]["speedScore"].get<double>() <= 1.0);
    CHECK(body["candidates"][0]["reasons"][2]
              .get<std::string>()
              .find("Actual peak VRAM") != std::string::npos);
}

TEST_CASE("Measured profile benchmark proves multi-request MTP before recommending it",
          "[gateway][dashboard][optimize][benchmark][mtp]") {
    TempConfig config;
    ConfigRouteServer routes(config);
    inferdeck::model::ModelInfo model;
    model.name = "test-mtp-27b";
    model.runtime = "llama_cpp";
    model.context_size = 100000;
    model.n_slots = 4;
    model.min_slots = 4;
    model.vram_required_mb = 24000;
    model.mtp_enabled = true;
    model.mtp_max_active_requests = 1;
    routes.registry.register_model(model);
    inferdeck::observability::GpuStats gpu;
    gpu.available = true;
    gpu.vram_total_mb = 32768.0;
    routes.gpu.record_external_sample(gpu);
    auto client = routes.client();
    const nlohmann::json request{
        {"model", model.name},
        {"contextPerSlot", model.context_size},
        {"slots", model.n_slots},
        {"minSlots", model.min_slots},
        {"nBatch", 2048},
        {"nUbatch", 2048},
        {"cacheTypeK", "q4_0"},
        {"cacheTypeV", "q4_0"},
        {"candidateLimit", 2},
    };

    const auto started = client.Post(
        "/api/inferdeck/v1/optimize/benchmark", request.dump(), "application/json");
    REQUIRE(started);
    REQUIRE(started->status == 202);
    REQUIRE(routes.profile_benchmark.wait_for_completion(
        std::chrono::seconds{2}));
    const auto response = client.Get("/api/inferdeck/v1/optimize/benchmark");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body["baseline"]["mtpMaxActiveRequests"] == 1);
    REQUIRE(body["candidates"].size() == 2);
    CHECK(body["candidates"][0]["mtpMaxActiveRequests"] == 2);
    CHECK(body["candidates"][1]["mtpMaxActiveRequests"] == 4);
    for (const auto& candidate : body["candidates"]) {
        const int window = candidate["mtpMaxActiveRequests"].get<int>();
        const auto verification = std::find_if(
            candidate["concurrency"].begin(),
            candidate["concurrency"].end(),
            [window](const auto& measured) {
                return measured["requests"].template get<int>() == window;
            });
        REQUIRE(verification != candidate["concurrency"].end());
        CHECK((*verification)["mtpRequests"] == window);
        CHECK((*verification)["mtpAcceptedTokens"].get<int>() > 0);
    }
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
        "/api/inferdeck/v1/optimize/benchmark", request.dump(), "application/json");
    REQUIRE(started);
    REQUIRE(started->status == 202);

    inferdeck::model::ModelInfo cpu_model;
    cpu_model.name = "whisper-test";
    cpu_model.runtime = "whisper_cpp";
    cpu_model.modality = "audio_transcription";
    cpu_model.vram_required_mb = 0;
    routes.registry.register_model(cpu_model);
    GatewayDeps resource_deps{
        routes.coordinator, "15", true, {}, 15000, nullptr, nullptr,
        nullptr, &routes.swap_tracker, &routes.maintenance_resource};
    CHECK(inferdeck::gateway::maintenance_blocks_model(resource_deps, "test-27b"));
    CHECK_FALSE(inferdeck::gateway::maintenance_blocks_model(resource_deps, "whisper-test"));

    const auto load = client.Post(
        "/api/inferdeck/v1/models/load", R"({"model":"test-27b"})",
        "application/json");
    REQUIRE(load);
    CHECK(load->status == 503);
    CHECK(nlohmann::json::parse(load->body)["error"]["code"] ==
          "maintenance_mode");

    const auto cancelled = client.Post(
        "/api/inferdeck/v1/optimize/benchmark/cancel", "{}", "application/json");
    REQUIRE(cancelled);
    CHECK(cancelled->status == 202);
    REQUIRE(routes.profile_benchmark.wait_for_completion(
        std::chrono::seconds{2}));
    const auto final = client.Get("/api/inferdeck/v1/optimize/benchmark");
    REQUIRE(final);
    const auto body = nlohmann::json::parse(final->body);
    CHECK(body["state"] == "cancelled");
    CHECK(body["restored"] == true);
    CHECK(routes.maintenance_resource.load() == ComputeResource::None);
    routes.maintenance_resource.store(ComputeResource::Cpu);
    CHECK_FALSE(inferdeck::gateway::maintenance_blocks_model(resource_deps, "test-27b"));
    CHECK(inferdeck::gateway::maintenance_blocks_model(resource_deps, "whisper-test"));
    routes.maintenance_resource.store(ComputeResource::None);
}
