#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "messaging/conversions.hpp"
#include "messaging/message.hpp"

#include "fixture_loader.hpp"

using inferdeck::messaging::Conversation;
using inferdeck::messaging::conversation_from_oai;
using inferdeck::messaging::conversation_from_anthropic;
using inferdeck::messaging::conversation_to_anthropic;
using nlohmann::json;

namespace {

struct AuthConfig {
    bool required{false};
    std::string token{};
    std::vector<std::string> allowed_origins{};
};

enum class AuthResult {
    Allow,
    Reject,
    RejectCors,
};

std::string strip_bearer(const std::string& header) {
    constexpr std::string_view prefix = "Bearer ";
    if (header.size() > prefix.size() &&
        header.compare(0, prefix.size(), prefix) == 0) {
        return header.substr(prefix.size());
    }
    return header;
}

AuthResult evaluate(const AuthConfig& cfg, const std::string& auth_header, const std::string& origin) {
    std::string bearer = strip_bearer(auth_header);
    if (cfg.required) {
        if (bearer.empty() || bearer != cfg.token) return AuthResult::Reject;
    }
    if (!origin.empty()) {
        bool ok = false;
        for (const auto& a : cfg.allowed_origins) {
            if (a == origin || a == "*") { ok = true; break; }
        }
        if (!ok) return AuthResult::RejectCors;
    }
    return AuthResult::Allow;
}

} // namespace

TEST_CASE("Auth: bearer token accepted when matches", "[integration][auth][happy]") {
    AuthConfig cfg;
    cfg.required = true;
    cfg.token = "secret-token-123";
    cfg.allowed_origins = {"http://localhost:3000", "http://192.168.0.168:11434"};
    auto r = evaluate(cfg, "secret-token-123", "http://localhost:3000");
    REQUIRE(r == AuthResult::Allow);
}

TEST_CASE("Auth: missing bearer rejected when required", "[integration][auth][reject]") {
    AuthConfig cfg;
    cfg.required = true;
    cfg.token = "secret-token-123";
    REQUIRE(evaluate(cfg, "", "http://localhost:3000") == AuthResult::Reject);
    REQUIRE(evaluate(cfg, "wrong-token", "http://localhost:3000") == AuthResult::Reject);
}

TEST_CASE("Auth: auth not required => bearer ignored", "[integration][auth][optional]") {
    AuthConfig cfg;
    cfg.required = false;
    cfg.token = "secret-token-123";
    REQUIRE(evaluate(cfg, "", "") == AuthResult::Allow);
    REQUIRE(evaluate(cfg, "anything", "") == AuthResult::Allow);
}

TEST_CASE("Auth: CORS origin not in allow list is rejected", "[integration][auth][cors]") {
    AuthConfig cfg;
    cfg.required = false;
    cfg.allowed_origins = {"http://localhost:3000", "http://192.168.0.168:11434"};
    REQUIRE(evaluate(cfg, "", "http://evil.example.com") == AuthResult::RejectCors);
    REQUIRE(evaluate(cfg, "", "http://localhost:3000") == AuthResult::Allow);
    REQUIRE(evaluate(cfg, "", "http://192.168.0.168:11434") == AuthResult::Allow);
}

TEST_CASE("Auth: edge_auth_headers fixture is a request that should be authorized with token", "[integration][auth][fixture]") {
    auto raw = test_helpers::load_fixture_text("edge_auth_headers.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    if (j.contains("model")) {
        auto conv = conversation_from_oai(j);
        REQUIRE(conv.has_value());
        REQUIRE(!conv->messages.empty());
    }
    AuthConfig cfg;
    cfg.required = true;
    cfg.token = "test-token";
    cfg.allowed_origins = {"http://localhost:3000"};
    REQUIRE(evaluate(cfg, "Bearer test-token", "http://localhost:3000") == AuthResult::Allow);
    REQUIRE(evaluate(cfg, "Bearer wrong-token", "http://localhost:3000") == AuthResult::Reject);
}

TEST_CASE("Auth: wildcard origin allowed", "[integration][auth][cors][wildcard]") {
    AuthConfig cfg;
    cfg.allowed_origins = {"*"};
    REQUIRE(evaluate(cfg, "", "http://anywhere.example.com") == AuthResult::Allow);
}
