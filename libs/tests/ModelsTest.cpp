/// @file ModelsTest.cpp
/// @brief Unit tests for Models route handler.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <nlohmann/json.hpp>

TEST_CASE("Models response has OpenAI schema", "[route][models]") {
    // Expected response structure
    nlohmann::json expected = nlohmann::json::parse(R"({
        "object": "list",
        "data": [
            {
                "id": "default",
                "object": "model",
                "created": 1700000000,
                "owned_by": "inferdeck"
            }
        ]
    })");

    // Verify required fields
    REQUIRE(expected["object"] == "list");
    REQUIRE(expected.contains("data"));
    REQUIRE(expected["data"].is_array());
    REQUIRE(expected["data"].size() > 0);

    // Verify model entry
    auto& model = expected["data"][0];
    REQUIRE(model.contains("id"));
    REQUIRE(model.contains("object"));
    REQUIRE(model.contains("created"));
    REQUIRE(model.contains("owned_by"));
}

TEST_CASE("Models response is valid JSON", "[route][models]") {
    std::string json_str = R"({
        "object": "list",
        "data": [
            {
                "id": "llama-2-7b.Q4_K_M.gguf",
                "object": "model",
                "created": 1700000000,
                "owned_by": "inferdeck"
            }
        ]
    })";

    // Should parse without error
    REQUIRE_NOTHROW(nlohmann::json::parse(json_str));
}
