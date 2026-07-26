#include <catch2/catch_test_macros.hpp>

#include "gateway/auth.hpp"
#include "gateway/cors.hpp"

using inferdeck::gateway::AuthConfig;
using inferdeck::gateway::AuthMiddleware;
using inferdeck::gateway::CorsMiddleware;

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
}
