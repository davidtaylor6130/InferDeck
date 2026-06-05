#include <catch2/catch_test_macros.hpp>

#include "gateway/auth.hpp"

using inferdeck::gateway::AuthConfig;
using inferdeck::gateway::AuthMiddleware;

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
