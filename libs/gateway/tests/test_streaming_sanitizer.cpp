#include <catch2/catch_test_macros.hpp>

#include "gateway/streaming_sanitizer.hpp"

#include <string>

using inferdeck::gateway::StreamingSanitizer;

TEST_CASE("StreamingSanitizer: plain text passes through", "[sanitizer]") {
    StreamingSanitizer s;
    auto r = s.on_token("Hello world");
    REQUIRE(r.content == "Hello world");
    REQUIRE(s.total_raw() == 11);
    REQUIRE(s.total_clean() == 11);
}

TEST_CASE("StreamingSanitizer: empty token emits nothing", "[sanitizer]") {
    StreamingSanitizer s;
    auto r = s.on_token("");
    REQUIRE(r.content.empty());
    REQUIRE(r.reasoning_content.empty());
    REQUIRE(s.total_raw() == 0);
}

TEST_CASE("StreamingSanitizer: <think>...</think> extracts reasoning", "[sanitizer]") {
    StreamingSanitizer s;
    auto a = s.on_token("Hello <think>secret</think> world");
    REQUIRE(a.content == "Hello  world");
    REQUIRE(a.reasoning_content == "secret");
    REQUIRE(a.is_thinking_start == true);
    REQUIRE(a.is_thinking_end == true);
}

TEST_CASE("StreamingSanitizer: thinking content goes to reasoning_content",
          "[sanitizer]") {
    StreamingSanitizer s;
    auto p1 = s.on_token("Hello ");
    REQUIRE(p1.content == "Hello ");
    auto p2 = s.on_token("<think>se");
    REQUIRE(p2.content.empty());
    REQUIRE(p2.is_thinking_start == true);
    REQUIRE(p2.reasoning_content == "se");
    auto p3 = s.on_token("cret</think>");
    REQUIRE(p3.reasoning_content == "cret");
    REQUIRE(p3.is_thinking_end == true);
    auto p4 = s.on_token("world");
    REQUIRE(p4.content == "world");
    REQUIRE(s.total_clean() == 17);
    REQUIRE(s.total_raw() == 32);
}

TEST_CASE("StreamingSanitizer: tag split across tokens is buffered", "[sanitizer][streaming]") {
    StreamingSanitizer s;
    auto r1 = s.on_token("Hello <th");
    REQUIRE(r1.content == "Hello ");
    auto r2 = s.on_token("ink>sec");
    REQUIRE(r2.is_thinking_start == true);
    REQUIRE(r2.reasoning_content == "sec");
    auto r3 = s.on_token("ret</think> world");
    REQUIRE(r3.reasoning_content == "ret");
    REQUIRE(r3.is_thinking_end == true);
    REQUIRE(r3.content == " world");
}

TEST_CASE("StreamingSanitizer: <think> never closed is buffered until finish()",
          "[sanitizer][streaming]") {
    StreamingSanitizer s;
    s.on_token("abc<think>xyz");
    auto r = s.on_token("123");
    REQUIRE(r.reasoning_content == "123");
    REQUIRE(r.content.empty());
    auto tail = s.finish();
    REQUIRE(tail.content.empty());
    REQUIRE(tail.reasoning_content.empty());
}

TEST_CASE("StreamingSanitizer: <think>-...-</think> extracts reasoning",
          "[sanitizer]") {
    StreamingSanitizer s;
    auto a = s.on_token("<think>hello</think>");
    REQUIRE(a.content.empty());
    REQUIRE(a.reasoning_content == "hello");
    REQUIRE(a.is_thinking_start == true);
    REQUIRE(a.is_thinking_end == true);
    auto b = s.on_token("world");
    REQUIRE(b.content == "world");
}

TEST_CASE("StreamingSanitizer: < not followed by tag is emitted as literal",
          "[sanitizer]") {
    StreamingSanitizer s;
    auto a = s.on_token("5 < 3 and 2 > 1");
    REQUIRE(a.content == "5 < 3 and 2 > 1");
}

TEST_CASE("StreamingSanitizer: random < at end waits for finish()", "[sanitizer]") {
    StreamingSanitizer s;
    s.on_token("5 <");
    auto r = s.on_token(" 3");
    REQUIRE_FALSE(r.content.empty());
    auto r2 = s.on_token(" 3");
    REQUIRE(r2.content == " 3");
}

TEST_CASE("StreamingSanitizer: finish() flushes remaining < that isn't a tag",
          "[sanitizer]") {
    StreamingSanitizer s;
    s.on_token("trailing <");
    auto tail = s.finish();
    REQUIRE(tail.content == "<");
}

TEST_CASE("StreamingSanitizer: O(n) per token in cumulative input size", "[sanitizer]") {
    StreamingSanitizer s;
    std::size_t total = 0;
    std::string all;
    for (int i = 0; i < 1000; ++i) {
        std::string tok = "tok" + std::to_string(i) + " ";
        all += tok;
        auto out = s.on_token(tok);
        total += out.content.size();
    }
    REQUIRE(total == all.size());
    REQUIRE(s.total_raw() == all.size());
    REQUIRE(s.total_clean() == all.size());
}

TEST_CASE("StreamingSanitizer: tool call markers extract name and args",
          "[sanitizer][tool_call]") {
    StreamingSanitizer s;
    auto r = s.on_token("text <|tool_call_begin|>{\"name\":\"get_weather\"}<|tool_call_end|> done");
    REQUIRE(r.content == "text  done");
    REQUIRE(r.is_tool_call_start == true);
    REQUIRE(r.is_tool_call == true);
    REQUIRE(r.is_tool_call_end == true);
}

TEST_CASE("StreamingSanitizer: tool call with streaming tokens", "[sanitizer][tool_call]") {
    StreamingSanitizer s;
    auto r1 = s.on_token("<|tool_call_begin|>{\"na");
    REQUIRE(r1.is_tool_call_start == true);
    REQUIRE(r1.content.empty());

    auto r2 = s.on_token("me\":\"fn\",\"arguments\":{}}<|tool_call_end|>");
    REQUIRE(r2.is_tool_call == true);
    REQUIRE(r2.is_tool_call_end == true);
}

TEST_CASE("StreamingSanitizer: thinking can be disabled", "[sanitizer]") {
    inferdeck::gateway::StreamingSanitizerConfig cfg;
    cfg.enable_thinking = false;
    cfg.reasoning_format = inferdeck::gateway::ReasoningFormat::Auto;
    StreamingSanitizer s(cfg);
    auto r = s.on_token("Hello <think>secret</think> world");
    REQUIRE(r.content == "Hello <think>secret</think> world");
    REQUIRE(r.reasoning_content.empty());
}

TEST_CASE("StreamingSanitizer: tool calls can be disabled", "[sanitizer]") {
    inferdeck::gateway::StreamingSanitizerConfig cfg;
    cfg.enable_tool_calls = false;
    StreamingSanitizer s(cfg);
    auto r = s.on_token("hi <|tool_call_begin|>stuff<|tool_call_end|> there");
    REQUIRE(r.content == "hi <|tool_call_begin|>stuff<|tool_call_end|> there");
    REQUIRE(r.is_tool_call == false);
}

TEST_CASE("StreamingSanitizer: <|tool_calls_section_begin|>/<|tool_calls_section_end|>",
          "[sanitizer]") {
    StreamingSanitizer s;
    auto r = s.on_token("before <|tool_calls_section_begin|>stuff<|tool_calls_section_end|> after");
    REQUIRE(r.content == "before  after");
    REQUIRE(r.is_tool_call_start == true);
    REQUIRE(r.is_tool_call_end == true);
}

TEST_CASE("StreamingSanitizer: <thinking>/</thinking> (Qwen style)", "[sanitizer]") {
    StreamingSanitizer s;
    auto r = s.on_token("a<thinking>b</thinking>c");
    REQUIRE(r.content == "ac");
    REQUIRE(r.reasoning_content == "b");
    REQUIRE(r.is_thinking_start == true);
    REQUIRE(r.is_thinking_end == true);
}
