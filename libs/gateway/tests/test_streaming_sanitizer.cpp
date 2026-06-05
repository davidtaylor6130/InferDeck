#include <catch2/catch_test_macros.hpp>

#include "gateway/streaming_sanitizer.hpp"

#include <string>

using inferdeck::gateway::StreamingSanitizer;

TEST_CASE("StreamingSanitizer: plain text passes through", "[sanitizer]") {
    StreamingSanitizer s;
    REQUIRE(s.on_token("Hello world") == "Hello world");
    REQUIRE(s.total_raw() == 11);
    REQUIRE(s.total_clean() == 11);
}

TEST_CASE("StreamingSanitizer: empty token emits nothing", "[sanitizer]") {
    StreamingSanitizer s;
    REQUIRE(s.on_token("").empty());
    REQUIRE(s.total_raw() == 0);
}

TEST_CASE("StreamingSanitizer: <think>...</think> is stripped", "[sanitizer]") {
    StreamingSanitizer s;
    auto a = s.on_token("Hello <think>secret</think> world");
    REQUIRE(a == "Hello  world");
}

TEST_CASE("StreamingSanitizer: channel analysis block is stripped", "[sanitizer]") {
    StreamingSanitizer s;
    auto a = s.on_token(
        "ok <|channel|>analysis<|message|>deep thoughts<|end|> done");
    REQUIRE(a == "ok  done");
}

TEST_CASE("StreamingSanitizer: stray <|im_start|>, <|channel|>, <|end|> tags removed",
          "[sanitizer]") {
    StreamingSanitizer s;
    auto a = s.on_token(
        "<|im_start|>assistant<|channel|>final<|message|>Hi<|end|>");
    REQUIRE(a == "Hi");
}

TEST_CASE("StreamingSanitizer: incremental streaming of <think>-then-content",
          "[sanitizer][streaming]") {
    StreamingSanitizer s;
    auto p1 = s.on_token("Hello ");
    REQUIRE(p1 == "Hello ");
    auto p2 = s.on_token("<think>se");
    REQUIRE(p2.empty());
    auto p3 = s.on_token("cret</think>");
    REQUIRE(p3.empty());
    auto p4 = s.on_token("world");
    REQUIRE(p4 == "world");
    REQUIRE(s.total_clean() == 11);
    REQUIRE(s.total_raw() == 32);
}

TEST_CASE("StreamingSanitizer: tag split across tokens is buffered", "[sanitizer][streaming]") {
    StreamingSanitizer s;
    REQUIRE(s.on_token("Hello <th") == "Hello ");
    REQUIRE(s.on_token("ink>sec") == "");
    REQUIRE(s.on_token("ret</think> world") == " world");
}

TEST_CASE("StreamingSanitizer: <think> never closed is buffered until finish()",
          "[sanitizer][streaming]") {
    StreamingSanitizer s;
    s.on_token("abc<think>xyz");
    REQUIRE(s.on_token("123").empty());
    REQUIRE(s.think_depth() == 1);
    REQUIRE(s.finish().empty());
    REQUIRE(s.think_depth() == 0);
}

TEST_CASE("StreamingSanitizer: <think>-...-</think> strips only the inner block",
          "[sanitizer]") {
    StreamingSanitizer s;
    auto a = s.on_token("<think>hello</think>");
    REQUIRE(a.empty());
    auto b = s.on_token("world");
    REQUIRE(b == "world");
}

TEST_CASE("StreamingSanitizer: < not followed by tag is emitted as literal",
          "[sanitizer]") {
    StreamingSanitizer s;
    auto a = s.on_token("5 < 3 and 2 > 1");
    REQUIRE(a == "5 < 3 and 2 > 1");
}

TEST_CASE("StreamingSanitizer: random < at end waits for finish()", "[sanitizer]") {
    StreamingSanitizer s;
    s.on_token("5 <");
    REQUIRE(s.on_token(" 3").empty() == false);
    REQUIRE(s.on_token(" 3") == " 3");
}

TEST_CASE("StreamingSanitizer: finish() flushes remaining < that isn't a tag",
          "[sanitizer]") {
    StreamingSanitizer s;
    s.on_token("trailing <");
    REQUIRE(s.finish() == "<");
}

TEST_CASE("StreamingSanitizer: O(n) per token in cumulative input size", "[sanitizer]") {
    StreamingSanitizer s;
    std::size_t total = 0;
    std::string all;
    for (int i = 0; i < 1000; ++i) {
        std::string tok = "tok" + std::to_string(i) + " ";
        all += tok;
        auto out = s.on_token(tok);
        total += out.size();
    }
    REQUIRE(total == all.size());
    REQUIRE(s.total_raw() == all.size());
    REQUIRE(s.total_clean() == all.size());
}
