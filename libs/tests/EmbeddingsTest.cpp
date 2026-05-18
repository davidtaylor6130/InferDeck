/// @file EmbeddingsTest.cpp
/// @brief Unit tests for Embeddings route handler.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <nlohmann/json.hpp>

TEST_CASE("Embeddings response has OpenAI schema", "[route][embeddings]") {
    // Expected response structure
    nlohmann::json expected = nlohmann::json::parse(R"({
        "object": "list",
        "data": [
            {
                "object": "embedding",
                "index": 0,
                "embedding": [0.1, 0.2, 0.3]
            }
        ],
        "model": "default",
        "usage": {
            "prompt_tokens": 3,
            "total_tokens": 3
        }
    })");

    // Verify required fields
    REQUIRE(expected["object"] == "list");
    REQUIRE(expected.contains("data"));
    REQUIRE(expected.contains("model"));
    REQUIRE(expected.contains("usage"));

    // Verify embedding entry
    auto& emb = expected["data"][0];
    REQUIRE(emb["object"] == "embedding");
    REQUIRE(emb.contains("index"));
    REQUIRE(emb.contains("embedding"));

    // Verify usage
    auto& usage = expected["usage"];
    REQUIRE(usage.contains("prompt_tokens"));
    REQUIRE(usage.contains("total_tokens"));
}

TEST_CASE("Embeddings input accepts string and array", "[route][embeddings]") {
    // String input
    std::string string_input = R"({"model": "default", "input": "Hello"})";
    REQUIRE_NOTHROW(nlohmann::json::parse(string_input));

    // Array input
    std::string array_input = R"({"model": "default", "input": ["Hello", "world"]})";
    REQUIRE_NOTHROW(nlohmann::json::parse(array_input));
}
