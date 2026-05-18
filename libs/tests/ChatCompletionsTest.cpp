/// @file ChatCompletionsTest.cpp
/// @brief Unit tests for ChatCompletions route handler.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "routes/ChatCompletions.hpp"

TEST_CASE("ValidateChatRequest accepts valid input", "[route][chat]") {
    std::string valid_request = R"({
        "model": "default",
        "messages": [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "Hello!"}
        ]
    })";

    std::string error = inferdeck::gateway::routes::ValidateChatRequest(valid_request);
    REQUIRE(error.empty());
}

TEST_CASE("ValidateChatRequest rejects empty body", "[route][chat]") {
    std::string error = inferdeck::gateway::routes::ValidateChatRequest("");
    REQUIRE(!error.empty());
    REQUIRE(error.find("body") != std::string::npos);
}

TEST_CASE("ValidateChatRequest rejects missing messages", "[route][chat]") {
    std::string invalid = R"({"model": "default"})";

    std::string error = inferdeck::gateway::routes::ValidateChatRequest(invalid);
    REQUIRE(!error.empty());
    REQUIRE(error.find("messages") != std::string::npos);
}

TEST_CASE("ValidateChatRequest rejects invalid role", "[route][chat]") {
    std::string invalid = R"({
        "messages": [
            {"role": "invalid", "content": "test"}
        ]
    })";

    std::string error = inferdeck::gateway::routes::ValidateChatRequest(invalid);
    REQUIRE(!error.empty());
    REQUIRE(error.find("role") != std::string::npos);
}

TEST_CASE("ValidateChatRequest accepts all valid roles", "[route][chat]") {
    std::vector<std::string> valid_roles = {"system", "user", "assistant", "tool"};

    for (const auto& role : valid_roles) {
        std::string valid = R"({"messages": [{"role": ")" + role + R"(", "content": "test"}]})";
        std::string error = inferdeck::gateway::routes::ValidateChatRequest(valid);
        REQUIRE(error.empty());
    }
}
