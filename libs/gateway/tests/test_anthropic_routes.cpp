#include <catch2/catch_test_macros.hpp>

#include "gateway/anthropic_routes.hpp"

#include <nlohmann/json.hpp>

using namespace inferdeck::gateway;
using nlohmann::json;

TEST_CASE("anthropic_to_openai: system + simple text", "[anthropic]") {
    json body = {
        {"model", "claude-sonnet-4-6"},
        {"max_tokens", 100},
        {"system", "You are helpful."},
        {"messages", json::array({
            {{"role", "user"}, {"content", "hi"}},
        })},
    };
    auto out = anthropic_to_openai(body, "qwen3.6-27b");
    REQUIRE(out["model"] == "qwen3.6-27b");
    REQUIRE(out["max_tokens"] == 100);
    REQUIRE(out["messages"].size() == 2);
    CHECK(out["messages"][0]["role"] == "system");
    CHECK(out["messages"][0]["content"] == "You are helpful.");
    CHECK(out["messages"][1]["role"] == "user");
    CHECK(out["messages"][1]["content"] == "hi");
}

TEST_CASE("anthropic_to_openai: system as content blocks", "[anthropic]") {
    json body = {
        {"max_tokens", 10},
        {"system", json::array({
            {{"type", "text"}, {"text", "part one"}},
            {{"type", "text"}, {"text", "part two"}},
        })},
        {"messages", json::array({{{"role", "user"}, {"content", "x"}}})},
    };
    auto out = anthropic_to_openai(body, "m");
    CHECK(out["messages"][0]["content"] == "part one\npart two");
}

TEST_CASE("anthropic_to_openai: tool definitions and tool_choice", "[anthropic]") {
    json body = {
        {"max_tokens", 10},
        {"messages", json::array({{{"role", "user"}, {"content", "x"}}})},
        {"tools", json::array({
            {{"name", "get_weather"},
             {"description", "Get weather"},
             {"input_schema", {{"type", "object"},
                               {"properties", {{"city", {{"type", "string"}}}}}}}},
        })},
        {"tool_choice", {{"type", "any"}}},
    };
    auto out = anthropic_to_openai(body, "m");
    REQUIRE(out["tools"].size() == 1);
    CHECK(out["tools"][0]["type"] == "function");
    CHECK(out["tools"][0]["function"]["name"] == "get_weather");
    CHECK(out["tools"][0]["function"]["parameters"]["type"] == "object");
    CHECK(out["tool_choice"] == "required");
}

TEST_CASE("anthropic_to_openai: tool_use/tool_result round trip", "[anthropic]") {
    json body = {
        {"max_tokens", 10},
        {"messages", json::array({
            {{"role", "user"}, {"content", "weather in SF?"}},
            {{"role", "assistant"}, {"content", json::array({
                {{"type", "text"}, {"text", "checking"}},
                {{"type", "tool_use"}, {"id", "toolu_1"}, {"name", "get_weather"},
                 {"input", {{"city", "SF"}}}},
            })}},
            {{"role", "user"}, {"content", json::array({
                {{"type", "tool_result"}, {"tool_use_id", "toolu_1"},
                 {"content", "sunny"}},
            })}},
        })},
    };
    auto out = anthropic_to_openai(body, "m");
    REQUIRE(out["messages"].size() == 3);

    const auto& assistant = out["messages"][1];
    CHECK(assistant["role"] == "assistant");
    CHECK(assistant["content"] == "checking");
    REQUIRE(assistant["tool_calls"].size() == 1);
    CHECK(assistant["tool_calls"][0]["id"] == "toolu_1");
    CHECK(assistant["tool_calls"][0]["function"]["name"] == "get_weather");
    CHECK(json::parse(assistant["tool_calls"][0]["function"]["arguments"].get<std::string>())
              == json({{"city", "SF"}}));

    const auto& tool_msg = out["messages"][2];
    CHECK(tool_msg["role"] == "tool");
    CHECK(tool_msg["tool_call_id"] == "toolu_1");
    CHECK(tool_msg["content"] == "sunny");
}

TEST_CASE("anthropic_to_openai: tool_result with block content", "[anthropic]") {
    json body = {
        {"max_tokens", 10},
        {"messages", json::array({
            {{"role", "user"}, {"content", json::array({
                {{"type", "tool_result"}, {"tool_use_id", "t1"},
                 {"content", json::array({{{"type", "text"}, {"text", "result text"}}})}},
                {{"type", "text"}, {"text", "continue"}},
            })}},
        })},
    };
    auto out = anthropic_to_openai(body, "m");
    REQUIRE(out["messages"].size() == 2);
    CHECK(out["messages"][0]["role"] == "tool");
    CHECK(out["messages"][0]["content"] == "result text");
    CHECK(out["messages"][1]["role"] == "user");
    CHECK(out["messages"][1]["content"] == "continue");
}

TEST_CASE("anthropic_to_openai: stop_sequences and sampling passthrough", "[anthropic]") {
    json body = {
        {"max_tokens", 7},
        {"temperature", 0.2},
        {"top_p", 0.9},
        {"top_k", 20},
        {"stop_sequences", json::array({"END"})},
        {"stream", true},
        {"messages", json::array({{{"role", "user"}, {"content", "x"}}})},
    };
    auto out = anthropic_to_openai(body, "m");
    CHECK(out["max_tokens"] == 7);
    CHECK(out["temperature"] == 0.2);
    CHECK(out["top_p"] == 0.9);
    CHECK(out["top_k"] == 20);
    CHECK(out["stop"] == json::array({"END"}));
    CHECK(out["stream"] == true);
}

TEST_CASE("anthropic_to_openai: thinking blocks dropped", "[anthropic]") {
    json body = {
        {"max_tokens", 10},
        {"messages", json::array({
            {{"role", "assistant"}, {"content", json::array({
                {{"type", "thinking"}, {"thinking", "secret"}, {"signature", "s"}},
                {{"type", "text"}, {"text", "answer"}},
            })}},
        })},
    };
    auto out = anthropic_to_openai(body, "m");
    REQUIRE(out["messages"].size() == 1);
    CHECK(out["messages"][0]["content"] == "answer");
    CHECK_FALSE(out["messages"][0].contains("tool_calls"));
}

TEST_CASE("anthropic_to_openai: image block to data URI", "[anthropic]") {
    json body = {
        {"max_tokens", 10},
        {"messages", json::array({
            {{"role", "user"}, {"content", json::array({
                {{"type", "text"}, {"text", "what is this"}},
                {{"type", "image"}, {"source", {{"type", "base64"},
                                                {"media_type", "image/png"},
                                                {"data", "AAAA"}}}},
            })}},
        })},
    };
    auto out = anthropic_to_openai(body, "m");
    const auto& parts = out["messages"][0]["content"];
    REQUIRE(parts.is_array());
    REQUIRE(parts.size() == 2);
    CHECK(parts[0]["type"] == "text");
    CHECK(parts[1]["type"] == "image_url");
    CHECK(parts[1]["image_url"]["url"] == "data:image/png;base64,AAAA");
}

TEST_CASE("anthropic_stop_reason mapping", "[anthropic]") {
    CHECK(anthropic_stop_reason("stop", false) == "end_turn");
    CHECK(anthropic_stop_reason("length", false) == "max_tokens");
    CHECK(anthropic_stop_reason("tool_calls", false) == "tool_use");
    CHECK(anthropic_stop_reason("stop", true) == "tool_use");
}
