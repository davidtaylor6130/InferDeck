#include <catch2/catch_test_macros.hpp>

#include "foundation/result.hpp"
#include "gateway/dashboard_routes.hpp"
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
using inferdeck::gateway::DashboardDeps;
using inferdeck::gateway::ComputeResource;
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
    std::function<inferdeck::foundation::Result<void>(const std::string&)> validate =
        [](const std::string&) { return Ok(); };

    explicit ConfigRouteServer(const TempConfig& config,
                               std::string pricing_file = {}) {
        GatewayDeps gateway_deps{
            coordinator, "15", true, {}, {}, 15000, nullptr, nullptr, nullptr,
            &swap_tracker, &maintenance_resource};
        gateway_deps.metrics = &metrics;
        gateway_deps.stats_db = &stats_db;
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

    const auto current_response = client.Get("/api/config");
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
        "/api/config", request.dump(), "application/json");
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

    const auto current_response = client.Get("/api/config");
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
        "/api/config", request.dump(), "application/json");
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

    const auto response = client.Get("/api/config");
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

    const nlohmann::json create{
        {"target", "concrete-model"},
        {"requiredContextSize", 16384},
        {"requiredCapabilities", nlohmann::json::array({"chat_completions"})},
    };
    const auto created = client.Put(
        "/api/model-aliases/stable-chat", create.dump(), "application/json");
    REQUIRE(created);
    REQUIRE(created->status == 201);
    const auto created_body = nlohmann::json::parse(created->body);
    CHECK(created_body["name"] == "stable-chat");
    CHECK(created_body["target"] == "concrete-model");
    CHECK(created_body["requiredContextSize"] == 16384);

    const auto listed = client.Get("/api/model-aliases");
    REQUIRE(listed);
    REQUIRE(listed->status == 200);
    CHECK(nlohmann::json::parse(listed->body)["aliases"].size() == 1);
    const auto persisted = YAML::Load(TempConfig::read(config.active));
    CHECK(TempConfig::read(config.active).find("# keep this operator note") !=
          std::string::npos);
    REQUIRE(persisted["model_aliases"]);
    CHECK(persisted["model_aliases"][0]["name"].as<std::string>() == "stable-chat");

    const auto removed = client.Delete("/api/model-aliases/stable-chat");
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
    const auto response = client.Get("/api/pricing");
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
    auto client = routes.client();
    const auto status_response = client.Get("/api/status");
    REQUIRE(status_response);
    REQUIRE(status_response->status == 200);
    CHECK(nlohmann::json::parse(status_response->body)
              ["dailyTokenUsageAllTime"] == false);
    const auto response = client.Get("/api/inferdeck/v1/usage/daily");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    CHECK(body["dailyTokenUsageAllTime"] == true);
    CHECK(std::any_of(
        body["dailyTokenUsage"].begin(), body["dailyTokenUsage"].end(),
        [](const auto& row) {
            return row.value("model", "") == "old-model" &&
                   row.value("bucket", "") == "2024-01-01";
        }));
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
    const auto removed = client.Post(
        "/api/model-store/unregister", R"({"model":"external-model"})",
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
        "/api/optimize/benchmark", request.dump(), "application/json");
    REQUIRE(started);
    REQUIRE(started->status == 202);
    REQUIRE(routes.profile_benchmark.wait_for_completion(
        std::chrono::seconds{2}));
    const auto response = client.Get("/api/optimize/benchmark");
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
        "/api/optimize/benchmark", request.dump(), "application/json");
    REQUIRE(started);
    REQUIRE(started->status == 202);

    inferdeck::model::ModelInfo cpu_model;
    cpu_model.name = "whisper-test";
    cpu_model.runtime = "whisper_cpp";
    cpu_model.modality = "audio_transcription";
    cpu_model.vram_required_mb = 0;
    routes.registry.register_model(cpu_model);
    GatewayDeps resource_deps{
        routes.coordinator, "15", true, {}, {}, 15000, nullptr, nullptr,
        nullptr, &routes.swap_tracker, &routes.maintenance_resource};
    CHECK(inferdeck::gateway::maintenance_blocks_model(resource_deps, "test-27b"));
    CHECK_FALSE(inferdeck::gateway::maintenance_blocks_model(resource_deps, "whisper-test"));

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
    CHECK(routes.maintenance_resource.load() == ComputeResource::None);
    routes.maintenance_resource.store(ComputeResource::Cpu);
    CHECK_FALSE(inferdeck::gateway::maintenance_blocks_model(resource_deps, "test-27b"));
    CHECK(inferdeck::gateway::maintenance_blocks_model(resource_deps, "whisper-test"));
    routes.maintenance_resource.store(ComputeResource::None);
}
