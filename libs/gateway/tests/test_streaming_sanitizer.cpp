#include <catch2/catch_test_macros.hpp>

#include "gateway/streaming_sanitizer.hpp"

#include <string>

using inferdeck::gateway::StreamingSanitizer;
using inferdeck::gateway::Utf8StreamBuffer;

TEST_CASE("Utf8StreamBuffer: complete text passes through", "[sanitizer][utf8]") {
    Utf8StreamBuffer buffer;
    REQUIRE(buffer.on_chunk("plain text") == "plain text");
    REQUIRE(buffer.empty());
    REQUIRE(buffer.finish().empty());
}

TEST_CASE("Utf8StreamBuffer: split multibyte sequences are held", "[sanitizer][utf8]") {
    Utf8StreamBuffer buffer;

    REQUIRE(buffer.on_chunk(std::string("A\xc3", 2)) == "A");
    REQUIRE(buffer.on_chunk(std::string("\xbc", 1)) == std::string("\xc3\xbc", 2));

    REQUIRE(buffer.on_chunk(std::string("\xe2\x82", 2)).empty());
    REQUIRE(buffer.on_chunk(std::string("\xac", 1)) == std::string("\xe2\x82\xac", 3));

    REQUIRE(buffer.on_chunk(std::string("\xf0\x9f\x92", 3)).empty());
    REQUIRE(buffer.on_chunk(std::string("\xa1", 1)) == std::string("\xf0\x9f\x92\xa1", 4));
    REQUIRE(buffer.empty());
}

TEST_CASE("Utf8StreamBuffer: malformed and incomplete bytes are not dropped", "[sanitizer][utf8]") {
    Utf8StreamBuffer buffer;
    REQUIRE(buffer.on_chunk(std::string("\x80", 1)) == std::string("\x80", 1));
    REQUIRE(buffer.on_chunk(std::string("\xe2\x82", 2)).empty());
    REQUIRE(buffer.finish() == std::string("\xe2\x82", 2));
    REQUIRE(buffer.empty());
}

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

TEST_CASE("StreamingSanitizer: <think>...</think> block is suppressed", "[sanitizer]") {
    StreamingSanitizer s;
    std::string out = s.on_token("Hello <think>secret</think> world");
    out += s.finish();
    REQUIRE(out == "Hello  world");
    REQUIRE(s.think_depth() == 0);
}

TEST_CASE("StreamingSanitizer: thinking suppressed across tokens", "[sanitizer][streaming]") {
    StreamingSanitizer s;
    REQUIRE(s.on_token("Hello ") == "Hello ");
    REQUIRE(s.on_token("<think>se").empty());
    REQUIRE(s.think_depth() == 1);
    REQUIRE(s.on_token("cret</think>").empty());
    REQUIRE(s.think_depth() == 0);
    REQUIRE(s.on_token("world") == "world");
}

TEST_CASE("StreamingSanitizer: tag split across tokens is buffered", "[sanitizer][streaming]") {
    StreamingSanitizer s;
    REQUIRE(s.on_token("Hello <th") == "Hello ");
    REQUIRE(s.on_token("ink>sec").empty());
    REQUIRE(s.think_depth() == 1);
    std::string out = s.on_token("ret</think> world");
    REQUIRE(out == " world");
}

TEST_CASE("StreamingSanitizer: unterminated <think> is dropped at finish", "[sanitizer][streaming]") {
    StreamingSanitizer s;
    REQUIRE(s.on_token("abc<think>xyz") == "abc");
    REQUIRE(s.on_token("123").empty());
    REQUIRE(s.finish().empty());
    REQUIRE(s.think_depth() == 0);
}

TEST_CASE("StreamingSanitizer: channel analysis block is suppressed", "[sanitizer]") {
    StreamingSanitizer s;
    std::string out = s.on_token("A<|channel|>analysis<|message|>hidden<|end|>B");
    out += s.finish();
    REQUIRE(out == "AB");
    REQUIRE(s.channel_depth() == 0);
}

TEST_CASE("StreamingSanitizer: control tags are stripped", "[sanitizer]") {
    StreamingSanitizer s;
    std::string out = s.on_token("<|im_start|>assistant text here<|end|>");
    out += s.finish();
    REQUIRE(out == " text here");
}

TEST_CASE("StreamingSanitizer: bare angle bracket passes through", "[sanitizer]") {
    StreamingSanitizer s;
    std::string out = s.on_token("a < b and 2 > 1");
    out += s.finish();
    REQUIRE(out == "a < b and 2 > 1");
}
