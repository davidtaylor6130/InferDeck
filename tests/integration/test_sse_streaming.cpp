#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "messaging/conversions.hpp"
#include "messaging/message.hpp"

#include "fixture_loader.hpp"

using inferdeck::messaging::Conversation;
using inferdeck::messaging::Message;
using inferdeck::messaging::TextContent;
using inferdeck::messaging::ToolCallContent;
using inferdeck::messaging::ToolResultContent;
using inferdeck::messaging::conversation_from_oai;
using inferdeck::messaging::conversation_to_oai;
using inferdeck::messaging::conversation_to_anthropic;
using inferdeck::messaging::conversation_from_anthropic;
using nlohmann::json;

namespace {

std::string join_text(const Conversation& c) {
    std::string out;
    for (const auto& m : c.messages) {
        for (const auto& part : m.content) {
            if (auto* t = std::get_if<TextContent>(&part)) {
                if (!out.empty()) out += "\n---\n";
                std::string role_str(inferdeck::messaging::to_string(m.role));
                out += "[" + role_str + "] ";
                out += t->text;
            }
        }
    }
    return out;
}

bool contains_role(const Conversation& c, inferdeck::messaging::Role r) {
    for (const auto& m : c.messages) {
        if (m.role == r) return true;
    }
    return false;
}

} // namespace

TEST_CASE("SSE: parse openai_chat_completion_chunk stream", "[integration][sse][oai]") {
    auto raw = test_helpers::load_fixture_text("oai_sse_streaming.jsonl");
    REQUIRE(raw.has_value());
    REQUIRE(!raw->empty());

    std::vector<json> chunks;
    std::size_t pos = 0;
    while (pos < raw->size()) {
        auto nl = raw->find('\n', pos);
        if (nl == std::string::npos) nl = raw->size();
        std::string line = raw->substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;
        if (line.find("[DONE]") != std::string::npos) continue;
        try {
            chunks.push_back(json::parse(line));
        } catch (const json::parse_error&) {
            FAIL("malformed SSE line: " + line);
        }
    }
    REQUIRE(chunks.size() >= 3);

    std::string assembled;
    std::string current_id;
    std::string current_model;
    for (const auto& c : chunks) {
        REQUIRE(c.contains("object"));
        REQUIRE(c["object"] == "chat.completion.chunk");
        REQUIRE(c.contains("id"));
        if (current_id.empty()) current_id = c["id"].get<std::string>();
        if (current_model.empty() && c.contains("model")) current_model = c["model"].get<std::string>();
        if (c.contains("choices") && c["choices"].is_array() && !c["choices"].empty()) {
            const auto& ch = c["choices"][0];
            if (ch.contains("delta") && ch["delta"].is_object()) {
                const auto& d = ch["delta"];
                if (d.contains("content") && d["content"].is_string()) {
                    assembled += d["content"].get<std::string>();
                }
            }
        }
    }
    REQUIRE(!current_id.empty());
    REQUIRE(!current_model.empty());
    REQUIRE(assembled.find("Servers") != std::string::npos);
    REQUIRE(assembled.find("whisper") != std::string::npos);
    REQUIRE(assembled.find("queues") != std::string::npos);
    REQUIRE(assembled.find("remember") != std::string::npos);
}

TEST_CASE("SSE: parse openai tool_call stream", "[integration][sse][oai][tools]") {
    auto raw = test_helpers::load_fixture_text("oai_sse_with_tool_call.jsonl");
    REQUIRE(raw.has_value());

    std::vector<json> chunks;
    std::size_t pos = 0;
    while (pos < raw->size()) {
        auto nl = raw->find('\n', pos);
        if (nl == std::string::npos) nl = raw->size();
        std::string line = raw->substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;
        if (line.find("[DONE]") != std::string::npos) continue;
        try {
            chunks.push_back(json::parse(line));
        } catch (const json::parse_error&) {
            continue;
        }
    }
    REQUIRE(!chunks.empty());

    std::string tool_id;
    std::string tool_name;
    bool found_tool_calls_field = false;
    std::string finish_reason;
    for (const auto& c : chunks) {
        if (!c.contains("choices") || c["choices"].empty()) continue;
        const auto& ch = c["choices"][0];
        if (!ch.contains("delta") || !ch["delta"].is_object()) continue;
        const auto& d = ch["delta"];
        if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
            found_tool_calls_field = true;
            for (const auto& tc : d["tool_calls"]) {
                if (tc.contains("id") && tc["id"].is_string()) tool_id = tc["id"].get<std::string>();
                if (tc.contains("function")) {
                    if (tc["function"].contains("name")) {
                        tool_name = tc["function"]["name"].get<std::string>();
                    }
                }
            }
        }
        if (ch.contains("finish_reason") && ch["finish_reason"].is_string()) {
            finish_reason = ch["finish_reason"].get<std::string>();
        }
    }
    REQUIRE(found_tool_calls_field);
    REQUIRE(!tool_id.empty());
    REQUIRE(!tool_name.empty());
    REQUIRE(finish_reason == "tool_calls");
}

TEST_CASE("SSE: streaming request parses as Conversation with stream=true", "[integration][sse][request]") {
    auto raw = test_helpers::load_fixture_text("oai_streaming_request.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    REQUIRE(j["stream"] == true);
    auto conv = conversation_from_oai(j);
    REQUIRE(conv.has_value());
    REQUIRE(conv->stream.value_or(false));
    REQUIRE(!conv->messages.empty());
}

TEST_CASE("SSE: chunk required fields present", "[integration][sse][schema]") {
    auto raw = test_helpers::load_fixture_text("oai_sse_streaming.jsonl");
    REQUIRE(raw.has_value());
    auto nl = raw->find('\n');
    REQUIRE(nl != std::string::npos);
    json c = json::parse(raw->substr(0, nl));
    REQUIRE(c.contains("id"));
    REQUIRE(c.contains("object"));
    REQUIRE(c.contains("created"));
    REQUIRE(c.contains("model"));
    REQUIRE(c.contains("choices"));
    REQUIRE(c["choices"].is_array());
    if (!c["choices"].empty()) {
        const auto& ch = c["choices"][0];
        REQUIRE(ch.contains("index"));
        REQUIRE(ch.contains("delta"));
        const bool has_finish = ch.contains("finish_reason");
        (void)has_finish;
    }
}
