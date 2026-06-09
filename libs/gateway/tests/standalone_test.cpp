// Standalone test for StreamingSanitizer - no DLL dependencies
#include "gateway/streaming_sanitizer.hpp"
#include <cassert>
#include <iostream>
#include <string>

using inferdeck::gateway::StreamingSanitizer;
using inferdeck::gateway::StreamingSanitizerConfig;
using inferdeck::gateway::ReasoningFormat;

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")" << std::endl; \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define CHECK_EQ(a, b, msg) CHECK((a) == (b), msg)

void test_plain_text() {
    StreamingSanitizer s;
    auto r = s.on_token("Hello world");
    CHECK_EQ(r.content, "Hello world", "plain text passes through");
    CHECK_EQ(r.reasoning_content, "", "no reasoning");
    CHECK_EQ(s.total_raw(), 11ull, "raw count");
    CHECK_EQ(s.total_clean(), 11ull, "clean count");
}

void test_empty_token() {
    StreamingSanitizer s;
    auto r = s.on_token("");
    CHECK(r.content.empty(), "empty token content");
    CHECK(r.reasoning_content.empty(), "empty token reasoning");
    CHECK_EQ(s.total_raw(), 0ull, "empty raw count");
}

void test_think_tag() {
    StreamingSanitizer s;
    auto a = s.on_token("Hello <think>secret</think> world");
    CHECK_EQ(a.content, "Hello  world", "think stripped from content");
    CHECK_EQ(a.reasoning_content, "secret", "think content in reasoning");
    CHECK(a.is_thinking_start, "thinking start flag");
    CHECK(a.is_thinking_end, "thinking end flag");
}

void test_think_tag_streamed() {
    StreamingSanitizer s;
    auto p1 = s.on_token("Hello ");
    CHECK_EQ(p1.content, "Hello ", "first part");
    auto p2 = s.on_token("<think>se");
    CHECK(p2.is_thinking_start, "think start flag on partial");
    CHECK(p2.reasoning_content.empty(), "no reasoning in start");
    auto p3 = s.on_token("cret</think>");
    CHECK_EQ(p3.reasoning_content, "cret", "reasoning content");
    CHECK(p3.is_thinking_end, "thinking end flag");
    auto p4 = s.on_token("world");
    CHECK_EQ(p4.content, "world", "content after think");
    CHECK_EQ(s.total_clean(), 11ull, "total clean");
    CHECK_EQ(s.total_raw(), 32ull, "total raw");
}

void test_tag_split_across_tokens() {
    StreamingSanitizer s;
    auto r1 = s.on_token("Hello <th");
    CHECK_EQ(r1.content, "Hello ", "content before partial tag");
    auto r2 = s.on_token("ink>sec");
    CHECK(r2.is_thinking_start, "think start on completed tag");
    CHECK(r2.reasoning_content.empty(), "no reasoning in second token");
    auto r3 = s.on_token("ret</think> world");
    CHECK_EQ(r3.reasoning_content, "ret", "reasoning in third");
    CHECK(r3.is_thinking_end, "think end flag");
    CHECK_EQ(r3.content, " world", "content after end");
}

void test_unclosed_think() {
    StreamingSanitizer s;
    s.on_token("abc<think>xyz");
    auto r = s.on_token("123");
    CHECK(r.reasoning_content.empty(), "no reasoning mid");
    CHECK(r.content.empty(), "no content mid");
    auto tail = s.finish();
    CHECK_EQ(tail.reasoning_content, "xyz123", "unclosed reasoning in finish");
    CHECK(tail.is_thinking_end, "thinking end in finish");
}

void test_think_only() {
    StreamingSanitizer s;
    auto a = s.on_token("<think>hello</think>");
    CHECK(a.content.empty(), "no content for pure think");
    CHECK_EQ(a.reasoning_content, "hello", "reasoning extracted");
    auto b = s.on_token("world");
    CHECK_EQ(b.content, "world", "content after think");
}

void test_literal_lt() {
    StreamingSanitizer s;
    auto a = s.on_token("5 < 3 and 2 > 1");
    CHECK_EQ(a.content, "5 < 3 and 2 > 1", "lt not treated as tag");
}

void test_trailing_lt() {
    StreamingSanitizer s;
    auto r = s.on_token("5 <");
    auto r2 = s.on_token(" 3");
    CHECK_EQ(r2.content, " 3", "lt not consumed");
}

void test_finish_flush() {
    StreamingSanitizer s;
    s.on_token("trailing <");
    auto tail = s.finish();
    CHECK_EQ(tail.content, "<", "finish flushes lt");
}

void test_think_disabled() {
    StreamingSanitizerConfig cfg;
    cfg.enable_thinking = false;
    StreamingSanitizer s(cfg);
    auto r = s.on_token("Hello <think>secret</think> world");
    CHECK_EQ(r.content, "Hello secret world", "think tags pass through");
    CHECK(r.reasoning_content.empty(), "no reasoning content");
}

void test_tool_call_disabled() {
    StreamingSanitizerConfig cfg;
    cfg.enable_tool_calls = false;
    StreamingSanitizer s(cfg);
    auto r = s.on_token("hi <|tool_call_begin|>stuff<|tool_call_end|> there");
    CHECK_EQ(r.content, "hi <|tool_call_begin|>stuff<|tool_call_end|> there", "tool call markers pass through");
    CHECK(!r.is_tool_call, "no tool call flag");
}

void test_tool_call_basic() {
    StreamingSanitizer s;
    auto r = s.on_token("text <|tool_call_begin|>{\"name\":\"fn\"}<|tool_call_end|> done");
    CHECK_EQ(r.content, "text  done", "tool call stripped from content");
    CHECK(r.is_tool_call_start, "tool call start flag");
    CHECK(r.is_tool_call_end, "tool call end flag");
    CHECK(r.is_tool_call, "is tool call flag");
}

void test_tool_call_streamed() {
    StreamingSanitizer s;
    auto r1 = s.on_token("<|tool_call_begin|>{\"na");
    CHECK(r1.is_tool_call_start, "tool call start");
    CHECK(r1.content.empty(), "no content in start");
    auto r2 = s.on_token("me\":\"fn\",\"arguments\":{}}<|tool_call_end|>");
    CHECK(r2.is_tool_call, "is tool call");
    CHECK(r2.is_tool_call_end, "tool call end");
}

void test_qwen_thinking() {
    StreamingSanitizer s;
    auto r = s.on_token("a<thinking>b</thinking>c");
    CHECK_EQ(r.content, "ac", "qwen think stripped");
    CHECK_EQ(r.reasoning_content, "b", "qwen reasoning");
}

void test_tool_call_section() {
    StreamingSanitizer s;
    auto r = s.on_token("before <|tool_calls_section_begin|>stuff<|tool_calls_section_end|> after");
    CHECK_EQ(r.content, "before  after", "section markers stripped");
    CHECK(r.is_tool_call_start, "section start flag");
    CHECK(r.is_tool_call_end, "section end flag");
}

void test_performance() {
    StreamingSanitizer s;
    std::size_t total = 0;
    std::string all;
    for (int i = 0; i < 1000; ++i) {
        std::string tok = "tok" + std::to_string(i) + " ";
        all += tok;
        auto out = s.on_token(tok);
        total += out.content.size();
    }
    CHECK_EQ(total, all.size(), "performance all emitted");
    CHECK_EQ(s.total_raw(), all.size(), "performance raw");
    CHECK_EQ(s.total_clean(), all.size(), "performance clean");
}

int main() {
    test_plain_text();
    test_empty_token();
    test_think_tag();
    test_think_tag_streamed();
    test_tag_split_across_tokens();
    test_unclosed_think();
    test_think_only();
    test_literal_lt();
    test_trailing_lt();
    test_finish_flush();
    test_think_disabled();
    test_tool_call_disabled();
    test_tool_call_basic();
    test_tool_call_streamed();
    test_qwen_thinking();
    test_tool_call_section();
    test_performance();

    std::cout << tests_passed << "/" << tests_run << " tests passed" << std::endl;
    return tests_passed == tests_run ? 0 : 1;
}
