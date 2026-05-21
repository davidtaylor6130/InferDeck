/// @file ChatCompletionsTest.cpp
/// @brief Unit tests for ChatCompletions route handler.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "routes/ChatCompletions.hpp"
#include <nlohmann/json.hpp>

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

TEST_CASE("Tool requests force non-streaming backend", "[route][chat][opencode]") {
    nlohmann::json request = {
        {"stream", true},
        {"messages", nlohmann::json::array({{{"role", "user"}, {"content", "write a file"}}})},
        {"tools", nlohmann::json::array({{{"type", "function"}, {"function", {{"name", "write"}}}}})}
    };

    REQUIRE(inferdeck::gateway::routes::ShouldForceNonStreamingBackend(request));

    request["tools"] = nlohmann::json::array();
    REQUIRE_FALSE(inferdeck::gateway::routes::ShouldForceNonStreamingBackend(request));
}

TEST_CASE("Synthetic stream emits tool calls and final done marker", "[route][chat][opencode]") {
    nlohmann::json response = {
        {"id", "chatcmpl-test"},
        {"object", "chat.completion"},
        {"created", 1700000000},
        {"model", "qwen3-coder"},
        {"choices", nlohmann::json::array({
            {
                {"index", 0},
                {"message", {
                    {"role", "assistant"},
                    {"tool_calls", nlohmann::json::array({
                        {
                            {"id", "call_1"},
                            {"type", "function"},
                            {"function", {{"name", "write"}, {"arguments", R"({"filePath":"hello.cpp","content":"hi"})"}}}
                        }
                    })}
                }},
                {"finish_reason", "tool_calls"}
            }
        })}
    };

    std::string stream = inferdeck::gateway::routes::BuildSyntheticChatCompletionStream(response);

    REQUIRE(stream.find("text/event-stream") == std::string::npos);
    REQUIRE(stream.find("data: ") != std::string::npos);
    REQUIRE(stream.find("\"tool_calls\"") != std::string::npos);
    REQUIRE(stream.find("\"finish_reason\":\"tool_calls\"") != std::string::npos);
    REQUIRE(stream.find("data: [DONE]") != std::string::npos);
}
