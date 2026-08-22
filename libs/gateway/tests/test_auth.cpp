#include <catch2/catch_test_macros.hpp>

#include "gateway/auth.hpp"
#include "gateway/cors.hpp"
#include "gateway/request_id.hpp"
#include "gateway/request_security.hpp"

using inferdeck::gateway::AuthConfig;
using inferdeck::gateway::AuthMiddleware;
using inferdeck::gateway::AuthorizationStatus;
using inferdeck::gateway::CorsMiddleware;
using inferdeck::gateway::RouteAuthConfig;
using inferdeck::gateway::RouteAuthorizer;
using inferdeck::gateway::RoutePrincipal;
using inferdeck::gateway::classify_route;
using inferdeck::gateway::cookie_value;
using inferdeck::gateway::ContentPolicy;
using inferdeck::gateway::RequestPolicy;
using inferdeck::gateway::RequestValidationStatus;
using inferdeck::gateway::request_id;
using inferdeck::gateway::request_policy;
using inferdeck::gateway::json_body_limit;
using inferdeck::gateway::server_keep_alive_timeout;
using inferdeck::gateway::server_request_read_deadline;
using inferdeck::gateway::server_read_timeout;
using inferdeck::gateway::server_write_timeout;
using inferdeck::gateway::valid_request_id;
using inferdeck::gateway::validate_request;
using inferdeck::gateway::validate_request_headers;

TEST_CASE("AuthMiddleware: not required allows all", "[auth]") {
    AuthConfig cfg;
    AuthMiddleware m(cfg);
    REQUIRE(m.check(""));
    REQUIRE(m.check("Bearer anything"));
    REQUIRE_FALSE(m.required());
}

TEST_CASE("AuthMiddleware: required rejects empty header", "[auth]") {
    AuthConfig cfg;
    cfg.required = true;
    cfg.token = "secret123";
    AuthMiddleware m(cfg);
    REQUIRE_FALSE(m.check(""));
    REQUIRE_FALSE(m.check("Bearer "));
    REQUIRE_FALSE(m.check("Bearer wrong"));
}

TEST_CASE("AuthMiddleware: required with missing configured token fails closed", "[auth]") {
    AuthConfig cfg;
    cfg.required = true;
    AuthMiddleware m(cfg);
    REQUIRE_FALSE(m.check(""));
    REQUIRE_FALSE(m.check("Bearer anything"));
}

TEST_CASE("AuthMiddleware: required accepts correct token", "[auth]") {
    AuthConfig cfg;
    cfg.required = true;
    cfg.token = "secret123";
    AuthMiddleware m(cfg);
    REQUIRE(m.check("Bearer secret123"));
}

TEST_CASE("AuthMiddleware: case-sensitive", "[auth]") {
    AuthConfig cfg;
    cfg.required = true;
    cfg.token = "secret";
    AuthMiddleware m(cfg);
    REQUIRE_FALSE(m.check("Bearer SECRET"));
    REQUIRE(m.check("Bearer secret"));
}

TEST_CASE("AuthMiddleware: header without Bearer prefix rejected", "[auth]") {
    AuthConfig cfg;
    cfg.required = true;
    cfg.token = "secret";
    AuthMiddleware m(cfg);
    REQUIRE_FALSE(m.check("secret"));
    REQUIRE_FALSE(m.check("Basic secret"));
    REQUIRE_FALSE(m.check("Token secret"));
}

TEST_CASE("RouteAuthorizer: OpenAI data-plane authentication is independent",
          "[auth][principal]") {
    RouteAuthConfig cfg;
    cfg.data_plane.required = true;
    cfg.data_plane.token = "openai-token";
    RouteAuthorizer authorizer(cfg);

    CHECK(authorizer.authorize(RoutePrincipal::OpenAIDataPlane, "", "192.168.1.20",
                               "192.168.1.10:11434", false) ==
          AuthorizationStatus::AuthenticationRequired);
    CHECK(authorizer.authorize(RoutePrincipal::OpenAIDataPlane,
                               "Bearer openai-token", "192.168.1.20",
                               "192.168.1.10:11434", false) ==
          AuthorizationStatus::Granted);
    CHECK(authorizer.authorize(RoutePrincipal::PublicStatus, "", "192.168.1.20",
                               "192.168.1.10:11434", false) ==
          AuthorizationStatus::Granted);
}

TEST_CASE("RouteAuthorizer: control plane defaults to loopback only",
          "[auth][principal]") {
    RouteAuthorizer authorizer(RouteAuthConfig{});

    CHECK(authorizer.authorize(RoutePrincipal::ControlWrite, "", "127.0.0.1",
                               "127.0.0.1:11434", false) ==
          AuthorizationStatus::Granted);
    CHECK(authorizer.authorize(RoutePrincipal::ControlRead, "", "::1",
                               "[::1]:11434", false) ==
          AuthorizationStatus::Granted);
    CHECK(authorizer.authorize(RoutePrincipal::DashboardSession, "", "192.168.1.20",
                               "192.168.1.10:11434", false) ==
          AuthorizationStatus::Forbidden);
    CHECK(authorizer.authorize(RoutePrincipal::ControlWrite, "", "127.0.0.1",
                               "admin.example", false) == AuthorizationStatus::Forbidden);
    CHECK(authorizer.authorize(RoutePrincipal::ControlWrite, "", "127.0.0.1",
                               "127.0.0.1:11434", true) == AuthorizationStatus::Forbidden);
    CHECK_FALSE(RouteAuthorizer::is_loopback_authority("127.evil.example"));
}

TEST_CASE("RouteAuthorizer: remote control requires its own credential",
          "[auth][principal]") {
    RouteAuthConfig cfg;
    cfg.data_plane.required = true;
    cfg.data_plane.token = "openai-token";
    cfg.control_allow_remote = true;
    cfg.control_token = "control-token";
    RouteAuthorizer authorizer(cfg);

    CHECK(authorizer.authorize(RoutePrincipal::ControlWrite, "", "192.168.1.20",
                               "192.168.1.10:11434", false) ==
          AuthorizationStatus::AuthenticationRequired);
    CHECK(authorizer.authorize(RoutePrincipal::ControlWrite,
                               "Bearer openai-token", "192.168.1.20",
                               "192.168.1.10:11434", false) ==
          AuthorizationStatus::AuthenticationRequired);
    CHECK(authorizer.authorize(RoutePrincipal::ControlWrite,
                               "Bearer control-token", "192.168.1.20",
                               "192.168.1.10:11434", false) ==
          AuthorizationStatus::Granted);

    cfg.control_allow_data_plane_token = true;
    RouteAuthorizer shared_authorizer(cfg);
    CHECK(shared_authorizer.authorize(RoutePrincipal::ControlWrite,
                                      "Bearer openai-token", "192.168.1.20",
                                      "192.168.1.10:11434", false) ==
          AuthorizationStatus::Granted);
    CHECK(authorizer.authorize(RoutePrincipal::DashboardSession, "",
                               "192.168.1.20", "192.168.1.10:11434", false) ==
          AuthorizationStatus::AuthenticationRequired);
    CHECK(authorizer.authorize(RoutePrincipal::DashboardSession,
                               "Bearer control-token", "192.168.1.20",
                               "192.168.1.10:11434", false) ==
          AuthorizationStatus::Granted);
}

TEST_CASE("Route classification separates data and control principals",
          "[auth][principal]") {
    CHECK(classify_route("GET", "/v1/health") ==
          RoutePrincipal::OpenAIDataPlane);
    CHECK(classify_route("POST", "/v1/chat/completions") ==
          RoutePrincipal::OpenAIDataPlane);
    CHECK(classify_route("GET", "/api/inferdeck/v1/health") ==
          RoutePrincipal::ControlRead);
    CHECK(classify_route("GET", "/api/inferdeck/v1/swap/status") ==
          RoutePrincipal::ControlRead);
    CHECK(classify_route("POST", "/api/inferdeck/v1/swap/cancel") ==
          RoutePrincipal::ControlWrite);
    CHECK(classify_route("GET", "/api/inferdeck/v1/status") ==
          RoutePrincipal::DashboardSession);
    CHECK(classify_route("GET", "/api/inferdeck/v1/usage/daily") ==
          RoutePrincipal::ControlRead);
    CHECK(classify_route("GET", "/api/inferdeck/v1/config") ==
          RoutePrincipal::ControlRead);
    CHECK(classify_route("PUT", "/api/inferdeck/v1/config") ==
          RoutePrincipal::ControlWrite);
}

TEST_CASE("Dashboard session cookie parsing is exact", "[auth][dashboard]") {
    CHECK(cookie_value("a=1; inferdeck_control=secret-token; b=2",
                       "inferdeck_control") == "secret-token");
    CHECK(cookie_value("inferdeck_control=first; inferdeck_control_extra=second",
                       "inferdeck_control") == "first");
    CHECK(cookie_value("inferdeck_control_extra=second", "inferdeck_control").empty());
}

TEST_CASE("Every mutating administrative route requires the control principal",
          "[auth][principal][matrix]") {
    const std::pair<std::string_view, std::string_view> routes[] = {
        {"POST", "/api/inferdeck/v1/swap/to/model"},
        {"POST", "/api/inferdeck/v1/swap/cancel"},
        {"POST", "/api/inferdeck/v1/media/jobs/1/cancel"},
        {"POST", "/api/inferdeck/v1/optimize/profile"},
        {"POST", "/api/inferdeck/v1/optimize/benchmark"},
        {"POST", "/api/inferdeck/v1/optimize/benchmark/cancel"},
        {"POST", "/api/inferdeck/v1/model-store/downloads"},
        {"POST", "/api/inferdeck/v1/model-store/downloads/1/cancel"},
        {"POST", "/api/inferdeck/v1/model-store/downloads/1/resume"},
        {"POST", "/api/inferdeck/v1/model-store/remove"},
        {"POST", "/api/inferdeck/v1/model-store/archive"},
        {"POST", "/api/inferdeck/v1/model-store/unregister"},
        {"PUT", "/api/inferdeck/v1/model-aliases/stable-chat"},
        {"DELETE", "/api/inferdeck/v1/model-aliases/stable-chat"},
        {"PUT", "/api/inferdeck/v1/config"},
        {"PUT", "/api/inferdeck/v1/config/active"},
        {"DELETE", "/api/inferdeck/v1/config/active"},
        {"POST", "/api/inferdeck/v1/models/load"},
        {"POST", "/api/inferdeck/v1/models/unload"},
    };

    RouteAuthConfig cfg;
    RouteAuthorizer local_only(cfg);
    for (const auto& [method, path] : routes) {
        INFO(method << ' ' << path);
        CHECK(classify_route(method, path) == RoutePrincipal::ControlWrite);
        CHECK(local_only.authorize(RoutePrincipal::ControlWrite, "", "192.168.1.20",
                                   "192.168.1.10:11434", false) ==
              AuthorizationStatus::Forbidden);
    }

    cfg.control_allow_remote = true;
    cfg.control_token = "control-token";
    RouteAuthorizer remote(cfg);
    for (const auto& [method, path] : routes) {
        const auto principal = classify_route(method, path);
        CHECK(remote.authorize(principal, "", "192.168.1.20",
                               "192.168.1.10:11434", false) ==
              AuthorizationStatus::AuthenticationRequired);
        CHECK(remote.authorize(principal, "Bearer control-token", "192.168.1.20",
                               "192.168.1.10:11434", false) ==
              AuthorizationStatus::Granted);
    }
}

TEST_CASE("Request identifiers accept only a bounded safe alphabet",
          "[auth][request-id]") {
    CHECK(request_id("client.request_123") == "client.request_123");
    CHECK_FALSE(valid_request_id(""));
    CHECK_FALSE(valid_request_id("contains space"));
    CHECK_FALSE(valid_request_id(std::string(65, 'a')));
    CHECK_FALSE(valid_request_id("client-\xC3\xA9"));

    const auto generated = request_id("contains space");
    CHECK(generated.starts_with("req_"));
    CHECK(valid_request_id(generated));
}

TEST_CASE("Request policies enforce endpoint body and media constraints",
          "[auth][request-policy]") {
    httplib::Request request;
    request.body = "{}";
    request.headers.emplace("Content-Type", "application/json; charset=utf-8");
    CHECK(validate_request(request, request_policy("POST", "/v1/chat/completions")) ==
          RequestValidationStatus::Allowed);
    CHECK(validate_request(request, request_policy("GET", "/v1/models")) ==
          RequestValidationStatus::BodyNotAllowed);

    request.headers.clear();
    request.headers.emplace("Content-Type", "text/plain");
    CHECK(validate_request(request, request_policy(
              "POST", "/api/inferdeck/v1/config")) ==
          RequestValidationStatus::UnsupportedMediaType);

    request.body = "four";
    request.headers.clear();
    request.headers.emplace("Content-Type", "application/json");
    CHECK(validate_request(request, RequestPolicy{3, ContentPolicy::Json}) ==
          RequestValidationStatus::PayloadTooLarge);

    request.body = "multipart";
    request.headers.clear();
    request.headers.emplace("Content-Type", "multipart/form-data; boundary=test");
    CHECK(validate_request(request,
                           request_policy("POST", "/v1/audio/transcriptions")) ==
          RequestValidationStatus::Allowed);
    CHECK(server_read_timeout.count() == 30);
    CHECK(server_write_timeout.count() == 30);
    CHECK(server_keep_alive_timeout.count() == 5);
    CHECK(server_request_read_deadline.count() == 30);
}

TEST_CASE("Request headers reject oversized and streaming bodies before buffering",
          "[auth][request-policy][headers]") {
    httplib::Request request;
    request.headers.emplace("Content-Length", std::to_string(json_body_limit + 1));
    request.headers.emplace("Content-Type", "application/json");
    CHECK(validate_request_headers(
              request, request_policy("POST", "/v1/chat/completions")) ==
          RequestValidationStatus::PayloadTooLarge);

    request.headers.clear();
    request.headers.emplace("Transfer-Encoding", "chunked");
    request.headers.emplace("Content-Type", "application/json");
    CHECK(validate_request_headers(
              request, request_policy("POST", "/v1/chat/completions")) ==
          RequestValidationStatus::UnsupportedTransferEncoding);

    request.headers.clear();
    CHECK(validate_request_headers(
              request, request_policy("POST", "/api/inferdeck/v1/swap/cancel", true)) ==
          RequestValidationStatus::UnsupportedMediaType);
    request.headers.emplace("Content-Type", "application/json");
    CHECK(validate_request_headers(
              request, request_policy("POST", "/api/inferdeck/v1/swap/cancel", true)) ==
          RequestValidationStatus::Allowed);

    request.headers.clear();
    request.headers.emplace("Content-Length", "not-a-number");
    CHECK(validate_request_headers(request, request_policy("GET", "/v1/models")) ==
          RequestValidationStatus::InvalidContentLength);
}

TEST_CASE("CorsMiddleware: echoes only an exact allowlisted origin", "[cors]") {
    CorsMiddleware cors({"http://127.0.0.1:11434", "http://localhost:11434"});
    httplib::Request allowed;
    allowed.headers.emplace("Origin", "http://localhost:11434");
    httplib::Response allowed_response;
    cors.apply(allowed, allowed_response);
    CHECK(allowed_response.get_header_value("Access-Control-Allow-Origin") ==
          "http://localhost:11434");
    CHECK(allowed_response.get_header_value("Vary") == "Origin");

    httplib::Request rejected;
    rejected.headers.emplace("Origin", "https://untrusted.example");
    httplib::Response rejected_response;
    cors.apply(rejected, rejected_response);
    CHECK_FALSE(rejected_response.has_header("Access-Control-Allow-Origin"));
}

TEST_CASE("CorsMiddleware: wildcard emits one valid wildcard origin", "[cors]") {
    CorsMiddleware cors({"*"});
    httplib::Request request;
    request.headers.emplace("Origin", "https://client.example");
    httplib::Response response;
    cors.apply(request, response);
    CHECK(response.get_header_value("Access-Control-Allow-Origin") == "*");
    CHECK(response.get_header_value("Access-Control-Allow-Headers").find("X-Api-Key") ==
          std::string::npos);
}
