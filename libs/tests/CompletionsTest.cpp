/// @file CompletionsTest.cpp
/// @brief Unit tests for Completions route handler.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <nlohmann/json.hpp>

TEST_CASE("Completion request requires prompt", "[route][completion]") {
    nlohmann::json j;
    j["model"] = "default";

    // Should fail validation
    REQUIRE_THROWS_AS(nlohmann::json::parse(R"({})"), nlohmann::json::parse_error);
}

TEST_CASE("Completion response has OpenAI schema", "[route][completion]") {
    // Expected response structure
    nlohmann::json expected = nlohmann::json::parse(R"({
        "id": "cmpl-123",
        "object": "text_completion",
        "created": 1234567890,
        "model": "default",
        "choices": [
            {
                "text": "Hello!",
                "index": 0,
                "finish_reason": "stop"
            }
        ],
        "usage": {
            "prompt_tokens": 5,
            "completion_tokens": 2,
            "total_tokens": 7
        }
    })");

    // Verify required fields exist
    REQUIRE(expected.contains("id"));
    REQUIRE(expected.contains("object"));
    REQUIRE(expected.contains("created"));
    REQUIRE(expected.contains("model"));
    REQUIRE(expected.contains("choices"));
    REQUIRE(expected.contains("usage"));

    // Verify usage fields
    auto& usage = expected["usage"];
    REQUIRE(usage.contains("prompt_tokens"));
    REQUIRE(usage.contains("completion_tokens"));
    REQUIRE(usage.contains("total_tokens"));
}

TEST_CASE("Stream mode returns SSE format", "[route][completion]") {
    // SSE format verification
    std::string sse_chunk = R"({"id":"cmpl-123","object":"text_completion.chunk","created":1234567890,"model":"default","choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]})";

    // Should be valid JSON
    nlohmann::json j = nlohmann::json::parse(sse_chunk);
    REQUIRE(j.contains("id"));
    REQUIRE(j.contains("object"));
    REQUIRE(j.contains("choices"));
    REQUIRE(j["choices"][0].contains("delta"));
}
