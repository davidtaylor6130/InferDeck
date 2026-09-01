#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "foundation/result.hpp"
#include "gateway/api_key_routes.hpp"
#include "gateway/api_key_store.hpp"
#include "gateway/auth.hpp"

using inferdeck::foundation::ErrorCode;
using inferdeck::gateway::ApiKeyStore;
using inferdeck::gateway::AuthorizationStatus;
using inferdeck::gateway::RouteAuthConfig;
using inferdeck::gateway::RouteAuthorizer;
using inferdeck::gateway::RoutePrincipal;
using inferdeck::gateway::handle_create_api_key;
using inferdeck::gateway::handle_list_api_keys;
using inferdeck::gateway::handle_revoke_api_key;
using inferdeck::gateway::handle_update_api_key;
using inferdeck::gateway::resolve_request_priority;

namespace {

struct TempApiKeyDb {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("inferdeck-api-keys-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path path = root / "api-keys.db";

    TempApiKeyDb() {
        std::filesystem::create_directories(root);
    }

    ~TempApiKeyDb() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

}

TEST_CASE("ApiKeyStore creates a one-time secret and authenticates its hash",
          "[auth][api-key]") {
    ApiKeyStore store(":memory:");
    REQUIRE(store.healthy());

    const auto created = store.create("overnight jobs", -40);
    REQUIRE(created);
    CHECK(created->key.starts_with("idk_"));
    CHECK(created->record.name == "overnight jobs");
    CHECK(created->record.priority == -40);
    CHECK_FALSE(created->record.id.empty());
    CHECK_FALSE(created->record.prefix.empty());
    CHECK(created->record.revoked_at_unix_ms == std::nullopt);

    const auto authenticated =
        store.authenticate_bearer("Bearer " + created->key);
    REQUIRE(authenticated);
    CHECK(authenticated->id == created->record.id);
    CHECK(authenticated->priority == -40);
    CHECK_FALSE(store.authenticate_bearer("Bearer " + created->key + "x"));
    CHECK_FALSE(store.authenticate_bearer(created->key));

    const auto records = store.list();
    REQUIRE(records);
    REQUIRE(records->size() == 1);
    CHECK(records->front().id == created->record.id);
}

TEST_CASE("ApiKeyStore persists only a hash and survives reopen",
          "[auth][api-key][persistence]") {
    TempApiKeyDb database;
    std::string key;
    std::string id;
    {
        ApiKeyStore store(database.path.string());
        REQUIRE(store.healthy());
        const auto created = store.create("persistent", 25);
        REQUIRE(created);
        key = created->key;
        id = created->record.id;
    }

    for (const auto& entry : std::filesystem::directory_iterator(database.root)) {
        if (entry.is_regular_file()) {
            CHECK(read_file(entry.path()).find(key) == std::string::npos);
        }
    }

    ApiKeyStore reopened(database.path.string());
    REQUIRE(reopened.healthy());
    const auto authenticated = reopened.authenticate_bearer("Bearer " + key);
    REQUIRE(authenticated);
    CHECK(authenticated->id == id);
    CHECK(authenticated->priority == 25);
}

TEST_CASE("ApiKeyStore updates server priority and revokes immediately",
          "[auth][api-key][priority]") {
    ApiKeyStore store(":memory:");
    const auto created = store.create("worker", -80);
    REQUIRE(created);

    const auto updated = store.update(
        created->record.id, std::optional<std::string>{"urgent worker"}, 70);
    REQUIRE(updated);
    CHECK(updated->name == "urgent worker");
    CHECK(updated->priority == 70);
    CHECK(resolve_request_priority(
              &store, "Bearer " + created->key, -100) == 70);
    CHECK(resolve_request_priority(&store, "Bearer invalid", 150) == 100);

    const auto invalid_priority = store.update(
        created->record.id, std::nullopt, 101);
    REQUIRE_FALSE(invalid_priority);
    CHECK(invalid_priority.error().code == ErrorCode::InvalidArgument);

    REQUIRE(store.revoke(created->record.id));
    CHECK_FALSE(store.authenticate_bearer("Bearer " + created->key));
    CHECK(resolve_request_priority(
              &store, "Bearer " + created->key, -100) == -100);
    REQUIRE(store.revoke(created->record.id));

    const auto records = store.list();
    REQUIRE(records);
    REQUIRE(records->size() == 1);
    CHECK(records->front().revoked_at_unix_ms.has_value());
}

TEST_CASE("Managed API keys grant data-plane access but never control access",
          "[auth][api-key][principal]") {
    auto store = std::make_shared<ApiKeyStore>(":memory:");
    const auto created = store->create("client", 10);
    REQUIRE(created);

    RouteAuthConfig config;
    config.data_plane.required = true;
    config.data_plane.token = "legacy-token";
    config.control_allow_remote = true;
    config.control_token = "control-token";
    RouteAuthorizer authorizer(config, store);
    const std::string bearer = "Bearer " + created->key;

    CHECK(authorizer.authorize(RoutePrincipal::OpenAIDataPlane, bearer,
                               "192.168.1.20", "192.168.1.10:11434", false) ==
          AuthorizationStatus::Granted);
    CHECK(authorizer.authorize(RoutePrincipal::ControlRead, bearer,
                               "192.168.1.20", "192.168.1.10:11434", false) ==
          AuthorizationStatus::AuthenticationRequired);
    CHECK(authorizer.authorize(RoutePrincipal::ControlWrite, bearer,
                               "192.168.1.20", "192.168.1.10:11434", false) ==
          AuthorizationStatus::AuthenticationRequired);
}

TEST_CASE("API key handlers never list the one-time secret",
          "[auth][api-key][routes]") {
    ApiKeyStore store(":memory:");
    httplib::Request create_request;
    create_request.body = R"({"name":"batch client","priority":-20})";
    httplib::Response create_response;
    handle_create_api_key(create_request, create_response, store);

    REQUIRE(create_response.status == 201);
    CHECK(create_response.get_header_value("Cache-Control") == "no-store");
    const auto created = nlohmann::json::parse(create_response.body);
    REQUIRE(created["key"].get<std::string>().starts_with("idk_"));
    const std::string id = created["id"].get<std::string>();

    httplib::Request list_request;
    httplib::Response list_response;
    handle_list_api_keys(list_request, list_response, store);
    REQUIRE(list_response.status == 200);
    const auto listed = nlohmann::json::parse(list_response.body);
    REQUIRE(listed["apiKeys"].size() == 1);
    CHECK_FALSE(listed["apiKeys"][0].contains("key"));

    httplib::Request update_request;
    update_request.body = R"({"priority":60})";
    httplib::Response update_response;
    handle_update_api_key(update_request, update_response, store, id);
    REQUIRE(update_response.status == 200);
    CHECK(nlohmann::json::parse(update_response.body)["priority"] == 60);

    httplib::Request revoke_request;
    httplib::Response revoke_response;
    handle_revoke_api_key(revoke_request, revoke_response, store, id);
    CHECK(revoke_response.status == 204);
}

TEST_CASE("API key handlers reject malformed management input",
          "[auth][api-key][routes][validation]") {
    ApiKeyStore store(":memory:");
    httplib::Request request;
    request.body = R"({"name":"","priority":101,"unexpected":true})";
    httplib::Response response;
    handle_create_api_key(request, response, store);
    CHECK(response.status == 400);

    httplib::Request missing_update;
    missing_update.body = R"({})";
    httplib::Response missing_response;
    handle_update_api_key(missing_update, missing_response, store, "missing");
    CHECK(missing_response.status == 400);

    httplib::Request huge_priority;
    huge_priority.body =
        R"({"name":"overflow","priority":18446744073709551615})";
    httplib::Response huge_response;
    handle_create_api_key(huge_priority, huge_response, store);
    CHECK(huge_response.status == 400);
}

TEST_CASE("API key operations fail closed when storage is unavailable",
          "[auth][api-key][unavailable]") {
    ApiKeyStore store("");
    REQUIRE_FALSE(store.healthy());
    const auto created = store.create("client", 0);
    REQUIRE_FALSE(created);
    CHECK(created.error().code == ErrorCode::Unavailable);
    const auto listed = store.list();
    REQUIRE_FALSE(listed);
    CHECK(listed.error().code == ErrorCode::Unavailable);
}
