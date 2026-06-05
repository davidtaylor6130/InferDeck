#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "messaging/conversions.hpp"
#include "messaging/content.hpp"
#include "messaging/message.hpp"

#include "fixture_loader.hpp"

using inferdeck::messaging::Conversation;
using inferdeck::messaging::Message;
using inferdeck::messaging::Role;
using inferdeck::messaging::TextContent;
using inferdeck::messaging::ImageContent;
using inferdeck::messaging::ToolCallContent;
using inferdeck::messaging::ToolResultContent;
using inferdeck::messaging::ReasoningContent;
using inferdeck::messaging::DeveloperContent;
using inferdeck::messaging::conversation_from_oai;
using inferdeck::messaging::conversation_to_oai;
using inferdeck::messaging::conversation_from_anthropic;
using inferdeck::messaging::conversation_to_anthropic;
using nlohmann::json;

namespace {

std::size_t count_content(const Conversation& c) {
    std::size_t n = 0;
    for (const auto& m : c.messages) n += m.content.size();
    return n;
}

bool has_role(const Conversation& c, Role r) {
    for (const auto& m : c.messages) if (m.role == r) return true;
    return false;
}

int count_tool_calls(const Conversation& c) {
    int n = 0;
    for (const auto& m : c.messages) n += static_cast<int>(m.tool_calls().size());
    return n;
}

} // namespace

TEST_CASE("Realistic: opencode coding tool call round-trips through OAI", "[integration][realistic][oai][tools]") {
    auto raw = test_helpers::load_fixture_text("opencode_coding_tool_call.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());

    auto conv = conversation_from_oai(j);
    REQUIRE(conv.has_value());
    REQUIRE(!conv->messages.empty());
    REQUIRE(has_role(*conv, Role::User));
    REQUIRE(has_role(*conv, Role::Assistant));
    REQUIRE(count_tool_calls(*conv) >= 1);

    auto back = conversation_to_oai(*conv);
    auto reparsed = conversation_from_oai(back);
    REQUIRE(reparsed.has_value());
    REQUIRE(reparsed->messages.size() == conv->messages.size());
}

TEST_CASE("Realistic: opencode planning long context survives round-trip", "[integration][realistic][oai][long]") {
    auto raw = test_helpers::load_fixture_text("opencode_planning_long_context.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());

    auto conv = conversation_from_oai(j);
    REQUIRE(conv.has_value());
    REQUIRE(conv->messages.size() >= 2);
    REQUIRE(conv->messages[0].role == Role::System);
    REQUIRE(conv->messages[1].role == Role::User);

    auto back = conversation_to_oai(*conv);
    auto reparsed = conversation_from_oai(back);
    REQUIRE(reparsed.has_value());
    REQUIRE(reparsed->messages.size() == conv->messages.size());
}

TEST_CASE("Realistic: openwebui vision multi-image parses images", "[integration][realistic][oai][vision]") {
    auto raw = test_helpers::load_fixture_text("openwebui_vision_multi_image.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());

    auto conv = conversation_from_oai(j);
    REQUIRE(conv.has_value());
    int image_count = 0;
    for (const auto& m : conv->messages) {
        for (const auto& c : m.content) {
            if (std::get_if<ImageContent>(&c)) ++image_count;
        }
    }
    REQUIRE(image_count >= 2);
}

TEST_CASE("Realistic: opencode vision QA follow-up handles image + text", "[integration][realistic][oai][vision]") {
    auto raw = test_helpers::load_fixture_text("opencode_vision_qa_followup.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    auto conv = conversation_from_oai(j);
    REQUIRE(conv.has_value());
    bool has_text = false, has_image = false;
    for (const auto& m : conv->messages) {
        for (const auto& c : m.content) {
            if (std::get_if<TextContent>(&c)) has_text = true;
            if (std::get_if<ImageContent>(&c)) has_image = true;
        }
    }
    REQUIRE(has_text);
    REQUIRE(has_image);
}

TEST_CASE("Realistic: anthropic claude coding with tool_use + tool_result", "[integration][realistic][anthropic][tools]") {
    auto raw = test_helpers::load_fixture_text("anthropic_claude_coding.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());

    auto conv = conversation_from_anthropic(j);
    REQUIRE(conv.has_value());
    REQUIRE(!conv->messages.empty());

    int tool_uses = 0, tool_results = 0;
    for (const auto& m : conv->messages) {
        for (const auto& c : m.content) {
            if (std::get_if<ToolCallContent>(&c)) ++tool_uses;
            if (std::get_if<ToolResultContent>(&c)) ++tool_results;
        }
    }
    REQUIRE(tool_uses >= 1);
    REQUIRE(tool_results >= 1);

    auto back = conversation_to_anthropic(*conv);
    auto reparsed = conversation_from_anthropic(back);
    REQUIRE(reparsed.has_value());
    REQUIRE(reparsed->messages.size() == conv->messages.size());
}

TEST_CASE("Realistic: anthropic claude vision includes image content", "[integration][realistic][anthropic][vision]") {
    auto raw = test_helpers::load_fixture_text("anthropic_claude_vision.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    auto conv = conversation_from_anthropic(j);
    REQUIRE(conv.has_value());
    int images = 0;
    for (const auto& m : conv->messages) {
        for (const auto& c : m.content) {
            if (std::get_if<ImageContent>(&c)) ++images;
        }
    }
    REQUIRE(images >= 1);
}

TEST_CASE("Edge: empty content message parses without crash", "[integration][edge]") {
    auto raw = test_helpers::load_fixture_text("edge_empty_content.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    auto conv = conversation_from_oai(j);
    REQUIRE(conv.has_value());
    REQUIRE(!conv->messages.empty());
}

TEST_CASE("Edge: unicode content preserved through round-trip", "[integration][edge][unicode]") {
    auto raw = test_helpers::load_fixture_text("edge_unicode.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    auto conv = conversation_from_oai(j);
    REQUIRE(conv.has_value());
    bool found_unicode = false;
    for (const auto& m : conv->messages) {
        for (const auto& c : m.content) {
            if (auto* t = std::get_if<TextContent>(&c)) {
                if (t->text.find("\xd9\x85") != std::string::npos ||
                    t->text.find("\xe8\xbf\x99") != std::string::npos ||
                    t->text.find("\xf0\x9f\x9a\x80") != std::string::npos ||
                    t->text.find("\xce\xb3") != std::string::npos) {
                    found_unicode = true;
                }
            }
        }
    }
    REQUIRE(found_unicode);
    auto back = conversation_to_oai(*conv);
    auto reparsed = conversation_from_oai(back);
    REQUIRE(reparsed.has_value());
    REQUIRE(reparsed->messages.size() == conv->messages.size());
}

TEST_CASE("Edge: huge context (208KB) parses within time budget", "[integration][edge][perf]") {
    auto raw = test_helpers::load_fixture_text("edge_huge_context.json");
    REQUIRE(raw.has_value());
    REQUIRE(raw->size() > 100000);

    auto start = std::chrono::steady_clock::now();
    auto j = json::parse(*raw, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    auto conv = conversation_from_oai(j);
    REQUIRE(conv.has_value());
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    REQUIRE(elapsed_ms < 5000);
    REQUIRE(conv->messages.size() >= 1);
}

TEST_CASE("Realistic: full OAI->internal->OAI round-trip preserves message count", "[integration][realistic][roundtrip]") {
    auto raw = test_helpers::load_fixture_text("opencode_coding_tool_call.json");
    REQUIRE(raw.has_value());
    auto j = json::parse(*raw, nullptr, false);
    auto conv = conversation_from_oai(j);
    REQUIRE(conv.has_value());
    auto out = conversation_to_oai(*conv);
    auto back = conversation_from_oai(out);
    REQUIRE(back.has_value());
    REQUIRE(back->messages.size() == conv->messages.size());
}
