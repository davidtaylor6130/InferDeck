/// @file test_llama_engine.cpp
/// @brief Unit tests for the LlamaEngine module (interface validation).

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "llama_cpp/LlamaEngine.hpp"

TEST_CASE("InferenceParams has correct defaults", "[engine][params]") {
    inferdeck::core::InferenceParams params;
    REQUIRE(params.temperature == 0.7f);
    REQUIRE(params.top_k == 40);
    REQUIRE(params.top_p == 0.9f);
    REQUIRE(params.max_tokens == 256);
    REQUIRE(params.min_tokens == 0);
    REQUIRE(!params.stream);
    REQUIRE(params.seed == -1);
}

TEST_CASE("InferenceResult has correct defaults", "[engine][result]") {
    inferdeck::core::InferenceResult result;
    REQUIRE(result.text.empty());
    REQUIRE(result.prompt_tokens == 0);
    REQUIRE(result.completion_tokens == 0);
    REQUIRE(result.total_tokens == 0);
    REQUIRE(result.duration_ms == 0);
    REQUIRE(result.tps == 0.0);
    REQUIRE(result.gpu_memory_used == 0);
}

TEST_CASE("ChatMessage role enum values", "[engine][role]") {
    REQUIRE(static_cast<int>(inferdeck::core::MessageRole::System) == 0);
    REQUIRE(static_cast<int>(inferdeck::core::MessageRole::User) == 1);
    REQUIRE(static_cast<int>(inferdeck::core::MessageRole::Assistant) == 2);
    REQUIRE(static_cast<int>(inferdeck::core::MessageRole::Tool) == 3);
}

TEST_CASE("ChatMessage constructor works", "[engine][message]") {
    auto msg = inferdeck::core::ChatMessage(
        inferdeck::core::MessageRole::User,
        "Hello, world!"
    );
    REQUIRE(msg.role == inferdeck::core::MessageRole::User);
    REQUIRE(msg.content == "Hello, world!");
}
